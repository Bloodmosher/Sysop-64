/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * DESCRIPTION:
 *     SID file playback engine (cSID-light by Hermit, wrapped in C++ class).
 *     Emulates the 6502 CPU and SID chip; produces per-frame SID register
 *     writes that can be applied to real C64 hardware via the sysop bridge.
 */

#include "sid_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <iomanip>

// Wrap the original C code in an anonymous namespace to avoid global pollution
namespace {

// --- Original csid-light.c content starts here (adapted) ---

// cSID by Hermit (Mihaly Horvath), (Year 2017) http://hermit.sidrip.com
// (based on jsSID, this version has much lower CPU-usage, as mainloop runs at samplerate)
// License: WTF - Do what the fuck you want with this code, but please mention me as its original author.

typedef unsigned char byte;

// Global constants and variables
#define C64_PAL_CPUCLK 985248.0
#define SID_CHANNEL_AMOUNT 3
#define MAX_FILENAME_LEN 512
#define MAX_DATA_LEN 65536
#define MAX_PLAYLIST_LEN 1000000
#define MAX_PLAYLIST_ROWLEN 600
//#define PAL_FRAMERATE 50.06 
#define PAL_FRAMERATE 50.125
#define DEFAULT_SAMPLERATE 44100.0 
#define CLOCK_RATIO_DEFAULT (C64_PAL_CPUCLK / DEFAULT_SAMPLERATE) 
#define VCR_SHUNT_6581 1500 
#define VCR_FET_TRESHOLD 192 
#define CAP_6581 0.470 
#define FILTER_DARKNESS_6581 22.0 
#define FILTER_DISTORTION_6581 0.0016 

int OUTPUT_SCALEDOWN = SID_CHANNEL_AMOUNT * 16 + 26; 

enum {
  GATE_BITMASK = 0x01,
  SYNC_BITMASK = 0x02,
  RING_BITMASK = 0x04,
  TEST_BITMASK = 0x08,
  TRI_BITMASK = 0x10,
  SAW_BITMASK = 0x20,
  PULSE_BITMASK = 0x40,
  NOISE_BITMASK = 0x80,
  HOLDZERO_BITMASK = 0x10,
  DECAYSUSTAIN_BITMASK = 0x40,
  ATTACK_BITMASK = 0x80,
  LOWPASS_BITMASK = 0x10,
  BANDPASS_BITMASK = 0x20,
  HIGHPASS_BITMASK = 0x40,
  OFF3_BITMASK = 0x80
};

float clock_ratio = CLOCK_RATIO_DEFAULT;

//  SID-emulation variables:
const byte FILTSW[9] = {1, 2, 4, 1, 2, 4, 1, 2, 4};
byte ADSRstate[9], expcnt[9], prevSR[9], sourceMSBrise[9];
short int envcnt[9];
unsigned int prevwfout[9], prevwavdata[9], sourceMSB[3], noise_LFSR[9];
int phaseaccu[9], prevaccu[9], prevlowpass[3], prevbandpass[3];
float ratecnt[9], cutoff_ratio_8580, cutoff_steepness_6581, cap_6581_reciprocal;

// Player-related variables:
int SIDamount = 1;
int SID_model[3] = {8580, 8580, 8580};
// int requested_SID_model = -1;
// int sampleratio;
byte filedata[MAX_DATA_LEN], memory[MAX_DATA_LEN];
byte timermode[0x20]; // , SIDtitle[0x20], SIDauthor[0x20], SIDinfo[0x20];
// char playlist[MAX_PLAYLIST_LEN] = "";
// char filename[MAX_FILENAME_LEN] = "";
int subtune = 0;
// int tunelength = -1;
// int default_tunelength = 300;
// int minutes = -1;
// int seconds = -1;
unsigned int initaddr, playaddr, playaddf;
unsigned int SID_address[3] = {0xD400, 0, 0};
int samplerate = DEFAULT_SAMPLERATE;
float framecnt = 0;
float frame_sampleperiod = DEFAULT_SAMPLERATE / PAL_FRAMERATE;
int filedata_len = 0;

// CPU (and CIA/VIC-IRQ) emulation constants and variables
const byte flagsw[] = {0x01, 0x21, 0x04, 0x24, 0x00, 0x40, 0x08, 0x28};
const byte branchflag[] = {0x80, 0x40, 0x01, 0x02};
unsigned int PC = 0, pPC = 0, addr = 0, storadd = 0;
short int A = 0, T = 0, SP = 0xFF;
byte X = 0, Y = 0, IR = 0, ST = 0x00; 
float CPUtime = 0.0;
char cycles = 0, finished = 0, dynCIA = 0;

// Capture buffer for the current frame
std::vector<SidRegisterWrite>* g_current_frame_pokes = nullptr;

// Per-voice sample capture for visualization
static int g_per_voice_out[3] = {0, 0, 0};
static SidVoiceSamples g_frame_voice_samples;

// --- CIA2-driven digi sample simulation ---
// When a SID tune arms CIA2 timer A and points the NMI vector at a handler
// that writes $D418 (volume DAC), we pre-simulate each NMI firing and emit
// raster-timed CMD_WAIT + CMD_POKE pairs so the DMA stream delivers the
// nibble stream at the correct cycle position within each PAL frame.
static bool     g_digi_enabled      = true;  // master on/off switch
static bool     g_digi_active       = false; // true when detected for current tune

// True when the tune routes its play call through the kernal system-IRQ vector
// ($0314/$0315) with CIA1 driving the rate.  In that case cSID's do_init() cannot
// observe CIA1 (the kernal sets it up at power-on, not in the tune init routine),
// so frame_sampleperiod falls back to the VBL rate (50Hz) instead of CIA1 (60Hz),
// making the tune play ~17% too slowly.
//
// The kernal programs CIA1 to exactly 60Hz on both PAL and NTSC:
//   PAL:  $DC04=$25 $DC05=$40 → latch=16421, period=16422 cycles @ 985248Hz  = 60.00Hz
//   NTSC: $DC04=$95 $DC05=$42 → latch=17045, period=17046 cycles @ 1022727Hz = 59.99Hz
//
// (Bus traces appear to show ~16667 because that is the period in *microseconds*
//  at ~1MHz; the actual CPU cycle count is ~16422.)
//   NTSC: $DC04=$95 $DC05=$42 → latch=17045, period=17046 cycles @ 1022727Hz = 59.99Hz
//
// (Bus traces appear to show ~16667 because that is the period in *microseconds*
//  at ~1MHz; the actual CPU cycle count is ~16422.)
constexpr float kKernalCia1CyclesPAL  = 16422.0f;  // CIA1 latch $4025 + 1
constexpr float kKernalCia1CyclesNTSC = 17046.0f;  // CIA1 latch $4295 + 1
constexpr float kNTSC_CPUCLK          = 1022727.0f;

// Named per-system timing constants.
// PAL:  312 lines × 63 cycles/line = 19 656 cycles/frame @ 985 248 Hz  = 50.125 Hz
// NTSC: 263 lines × 65 cycles/line = 17 095 cycles/frame @ 1 022 727 Hz = 59.826 Hz
constexpr float kPAL_CPUCLK       = 985248.0f;
constexpr float kPAL_FRAMERATE    = 50.125f;
constexpr float kPAL_FRAME_CYCLES = 19656.0f;
constexpr float kPAL_LINE_CYCLES  = 63.0f;
constexpr int   kPAL_TOTAL_LINES  = 312;
constexpr float kNTSC_FRAMERATE    = 59.826f;   // 1 022 727 / 17 095
constexpr float kNTSC_FRAME_CYCLES = 17095.0f;
constexpr float kNTSC_LINE_CYCLES  = 65.0f;
constexpr int   kNTSC_TOTAL_LINES  = 263;

// Active machine selection — set by SidPlayer before calling cSID_init / do_init.
static bool g_is_pal = true;
// Video standard from SID header flags bits 2-3 (+0x76, v2+):
// 0=Unknown/v1, 1=PAL, 2=NTSC, 3=PAL+NTSC.  Set by internal_init_from_memory().
static int  g_sid_clock_hint = 0;

static bool     g_uses_kernal_cia1    = false;
static uint16_t g_digi_timer_period = 0;     // CIA2 timer A period in cycles
static uint16_t g_nmi_addr          = 0;     // address of the NMI handler
static float    g_cia2_timer_accum  = 0.0f;  // fractional-cycle carry into next frame
static float    g_digi_carry_inc_accum = -1.0f; // when >= 0: absolute cycle within next frame where a cross-frame INC $D020 must fire

// Scan the NMI handler for STA $D418 (8D 18 D4) within the first SCAN bytes.
// Must be called after the play routine has run at least once (lazy detection)
// because RSID tunes set up CIA2 inside the IRQ handler, not in do_init().
static void detect_cia2_digi() {
    g_digi_active = false;
    if (!g_digi_enabled) return;
    if (!(memory[0xDD0E] & 0x01)) return;                       // CIA2 timer A not running
    uint16_t vec = memory[0xFFFA] | (memory[0xFFFB] << 8);
    if (vec < 0x0200 || vec >= 0xE000) return;                  // not a RAM NMI vector
    // Scan up to 80 bytes for STA $D418 absolute (opcode 8D 18 D4).
    // The Skate or Die handler has it at offset 66 — 64 would miss it.
    for (int s = 0; s < 80; s++) {
        uint16_t a = (vec + s) & 0xFFFF;
        if (memory[a] == 0x8D && memory[(a+1)&0xFFFF] == 0x18 && memory[(a+2)&0xFFFF] == 0xD4) {
            g_digi_active       = true;
            g_digi_timer_period = memory[0xDD04] | (memory[0xDD05] << 8);
            g_nmi_addr          = vec;
            return;
        }
    }
}

// Forward declarations needed by run_nmi (defined further down in this namespace)
void initCPU(unsigned int mempos);
byte CPU();

// Run the NMI handler at nmi_vec once, capturing any $D418 writes into
// g_current_frame_pokes. CPU state is fully saved/restored so the live
// play routine's registers are unaffected. The emulated memory[] carries
// the self-modifying sample pointer and nibble-alternation state forward
// between calls automatically.
static void run_nmi(uint16_t nmi_vec) {
    // Save live CPU state
    short int sA = A, sT = T;
    byte sX = X, sY = Y, sIR = IR, sST = ST;
    short int sSP = SP;
    unsigned int sPC = PC;
    float sCPUtime = CPUtime;
    char sfinished = finished;
    // Push return address and status register onto the emulated stack
    // (mirrors what the 6502 hardware does on NMI entry)
    memory[0x100 + (SP & 0xFF)] = (PC >> 8) & 0xFF; SP = (SP - 1) & 0xFF;
    memory[0x100 + (SP & 0xFF)] = PC & 0xFF;         SP = (SP - 1) & 0xFF;
    memory[0x100 + (SP & 0xFF)] = (byte)ST;          SP = (SP - 1) & 0xFF;
    // Jump to handler and run until RTI (returns 0xFF) or guard exhausted
    initCPU(nmi_vec);
    for (int guard = 0; guard < 5000; guard++) {
        if (CPU() >= 0xFE) break;
    }
    // Restore live CPU state
    A = sA; T = sT; X = sX; Y = sY; IR = sIR; ST = sST;
    SP = sSP; PC = sPC; CPUtime = sCPUtime; finished = sfinished;
}

// Serialises concurrent internal_init_from_memory calls (anonymous-namespace globals
// are shared; rapid back-to-back subtune switches can spawn two init threads briefly).
// This is a separate, lighter mutex from SidPlayer::sid_mutex: it is never held
// during play_frame(), so it cannot cause render-thread frame deadline misses.
static std::mutex g_sid_init_mutex;

// Function prototypes
void cSID_init(int samplerate);
int SID(char num, unsigned int baseaddr);
void initSID();
void initCPU(unsigned int mempos);
byte CPU();
unsigned int combinedWF(char num, char channel, unsigned int* wfarray, int index, char differ6581, byte freq);
void createCombinedWF(unsigned int* wfarray, float bitmul, float bitstrength, float treshold);
void do_init(byte subt);

// --------------------------------- CPU emulation -------------------------------------------

void initCPU(unsigned int mempos)
{
  PC = mempos;
  A = 0;
  X = 0;
  Y = 0;
  ST = 0;
  SP = 0xFF;
}

byte CPU()  
{
  IR = memory[PC];
  cycles = 2;
  storadd = 0; 

  if (IR & 1) {  
    switch (IR & 0x1F) {  
      case 1: case 3: PC++; addr = memory[memory[PC] + X] + memory[memory[PC] + X + 1] * 256; cycles = 6; break;  
      case 0x11: case 0x13: PC++; addr = memory[memory[PC]] + memory[memory[PC] + 1] * 256 + Y; cycles = 6; break;  
      case 0x19: case 0x1B: PC++; addr = memory[PC]; PC++; addr += memory[PC] * 256 + Y; cycles = 5; break;  
      case 0x1D: PC++; addr = memory[PC]; PC++; addr += memory[PC] * 256 + X; cycles = 5; break;  
      case 0xD: case 0xF: PC++; addr = memory[PC]; PC++; addr += memory[PC] * 256; cycles = 4; break;  
    case 0x15: PC++; addr = memory[PC] + X; cycles=4; break; 
    case 5: case 7: PC++; addr = memory[PC]; cycles=3; break; 
    case 0x17: PC++; if ((IR&0xC0)!=0x80) { addr = memory[PC] + X; cycles=4; } 
               else { addr = memory[PC] + Y; cycles=4; }  break; 
    case 0x1F: PC++; if ((IR&0xC0)!=0x80) { addr = memory[PC]; addr += memory[++PC]*256 + X; cycles=5; } 
               else { addr = memory[PC]; addr += memory[++PC]*256 + Y; cycles=5; }  break; 
    case 9: case 0xB: PC++; addr = PC; cycles=2;  
   }
   addr&=0xFFFF;
   switch (IR&0xE0) {
    case 0x60: if ((IR&0x1F)!=0xB) { if((IR&3)==3) {T=(memory[addr]>>1)+(ST&1)*128; ST&=124; ST|=(T&1); memory[addr]=T; cycles+=2;}   
                T=A; A+=memory[addr]+(ST&1); ST&=60; ST|=(A&128)|(A>255); A&=0xFF; ST |= (!A)<<1 | ( (!((T^memory[addr])&0x80)) & ((T^A)&0x80) ) >> 1; }
               else { A&=memory[addr]; T+=memory[addr]+(ST&1); ST&=60; ST |= (T>255) | ( (!((A^memory[addr])&0x80)) & ((T^A)&0x80) ) >> 1; 
                T=A; A=(A>>1)+(ST&1)*128; ST|=(A&128)|(T>127); ST|=(!A)<<1; }  break; 
    case 0xE0: if((IR&3)==3 && (IR&0x1F)!=0xB) {memory[addr]++;cycles+=2;}  T=A; A-=memory[addr]+!(ST&1); 
               ST&=60; ST|=(A&128)|(A>=0); A&=0xFF; ST |= (!A)<<1 | ( ((T^memory[addr])&0x80) & ((T^A)&0x80) ) >> 1; break; 
    case 0xC0: if((IR&0x1F)!=0xB) { if ((IR&3)==3) {memory[addr]--; cycles+=2;}  T=A-memory[addr]; } 
               else {X=T=(A&X)-memory[addr];}   ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0);  break;  
    case 0x00: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {ST&=124; ST|=(memory[addr]>127); memory[addr]<<=1; cycles+=2;}  
                A|=memory[addr]; ST&=125;ST|=(!A)<<1|(A&128); } 
               else {A&=memory[addr]; ST&=124;ST|=(!A)<<1|(A&128)|(A>127);}  break; 
    case 0x20: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {T=(memory[addr]<<1)+(ST&1); ST&=124; ST|=(T>255); T&=0xFF; memory[addr]=T; cycles+=2;}  
                A&=memory[addr]; ST&=125; ST|=(!A)<<1|(A&128); }  
               else {A&=memory[addr]; ST&=124;ST|=(!A)<<1|(A&128)|(A>127);}  break; 
    case 0x40: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {ST&=124; ST|=(memory[addr]&1); memory[addr]>>=1; cycles+=2;}
                A^=memory[addr]; ST&=125;ST|=(!A)<<1|(A&128); } 
                else {A&=memory[addr]; ST&=124; ST|=(A&1); A>>=1; A&=0xFF; ST|=(A&128)|((!A)<<1); }  break; 
    case 0xA0: if ((IR&0x1F)!=0x1B) { A=memory[addr]; if((IR&3)==3) X=A; } 
               else {A=X=SP=memory[addr]&SP;}   ST&=125; ST|=((!A)<<1) | (A&128); break;  
    case 0x80: if ((IR&0x1F)==0xB) { A = X & memory[addr]; ST&=125; ST|=(A&128) | ((!A)<<1); } 
               else if ((IR&0x1F)==0x1B) { SP=A&X; memory[addr]=SP&((addr>>8)+1); } 
               else 
               {
                // --- MODIFIED SECTION START ---
                if (addr >= 0xd400 && addr <= 0xd41c)
                {
                  if (g_current_frame_pokes) {
                      g_current_frame_pokes->push_back({(uint16_t)addr, (uint8_t)A});
                  }
                }

                // --- MODIFIED SECTION END ---
                
                memory[addr]=A & (((IR&3)==3)?X:0xFF); 
                storadd=addr;
                }  break; 
   }
  }
  
  else if(IR&2) {  
   switch (IR&0x1F) { 
    case 0x1E: PC++; addr=memory[PC]; PC++; addr+=memory[PC]*256 + ( ((IR&0xC0)!=0x80) ? X:Y ); cycles=5; break; 
    case 0xE: PC++; addr=memory[PC]; PC++; addr+=memory[PC]*256; cycles=4; break; 
    case 0x16: PC++; addr = memory[PC] + ( ((IR&0xC0)!=0x80) ? X:Y ); cycles=4; break; 
    case 6: PC++; addr = memory[PC]; cycles=3; break; 
    case 2: PC++; addr = PC; cycles=2;  
   }  
   addr&=0xFFFF; 
   switch (IR&0xE0) {
    case 0x00: ST&=0xFE; case 0x20: if((IR&0xF)==0xA) { A=(A<<1)+(ST&1); ST&=124;ST|=(A&128)|(A>255); A&=0xFF; ST|=(!A)<<1; } 
      else { T=(memory[addr]<<1)+(ST&1); ST&=124;ST|=(T&128)|(T>255); T&=0xFF; ST|=(!T)<<1; memory[addr]=T; cycles+=2; }  break; 
    case 0x40: ST&=0xFE; case 0x60: if((IR&0xF)==0xA) { T=A; A=(A>>1)+(ST&1)*128; ST&=124;ST|=(A&128)|(T&1); A&=0xFF; ST|=(!A)<<1; } 
      else { T=(memory[addr]>>1)+(ST&1)*128; ST&=124;ST|=(T&128)|(memory[addr]&1); T&=0xFF; ST|=(!T)<<1; memory[addr]=T; cycles+=2; }  break; 
    case 0xC0: if(IR&4) { memory[addr]--; ST&=125;ST|=(!memory[addr])<<1|(memory[addr]&128); cycles+=2; } 
      else {X--; X&=0xFF; ST&=125;ST|=(!X)<<1|(X&128);}  break; 
    case 0xA0: if((IR&0xF)!=0xA) X=memory[addr];  else if(IR&0x10) {X=SP;break;}  else X=A;  ST&=125;ST|=(!X)<<1|(X&128);  break; 
    case 0x80: if(IR&4) {
        if (addr >= 0xd400 && addr <= 0xd41c && g_current_frame_pokes) {
             g_current_frame_pokes->push_back({(uint16_t)addr, (uint8_t)X});
        }
        memory[addr]=X;storadd=addr;}  else if(IR&0x10) SP=X;  else {A=X; ST&=125;ST|=(!A)<<1|(A&128);}  break; 
    case 0xE0: if(IR&4) { memory[addr]++; ST&=125;ST|=(!memory[addr])<<1|(memory[addr]&128); cycles+=2; } 
   }
  }
  
  else if((IR&0xC)==8) {  
   switch (IR&0xF0) {
    case 0x60: SP++; SP&=0xFF; A=memory[0x100+SP]; ST&=125;ST|=(!A)<<1|(A&128); cycles=4; break; 
    case 0xC0: Y++; Y&=0xFF; ST&=125;ST|=(!Y)<<1|(Y&128); break; 
    case 0xE0: X++; X&=0xFF; ST&=125;ST|=(!X)<<1|(X&128); break; 
    case 0x80: Y--; Y&=0xFF; ST&=125;ST|=(!Y)<<1|(Y&128); break; 
    case 0x00: memory[0x100+SP]=ST; SP--; SP&=0xFF; cycles=3; break; 
    case 0x20: SP++; SP&=0xFF; ST=memory[0x100+SP]; cycles=4; break; 
    case 0x40: memory[0x100+SP]=A; SP--; SP&=0xFF; cycles=3; break; 
    case 0x90: A=Y; ST&=125;ST|=(!A)<<1|(A&128); break; 
    case 0xA0: Y=A; ST&=125;ST|=(!Y)<<1|(Y&128); break; 
    default: if(flagsw[IR>>5]&0x20) ST|=(flagsw[IR>>5]&0xDF); else ST&=255-(flagsw[IR>>5]&0xDF);  
   }
  }
  
  else {  
   if ((IR&0x1F)==0x10) { PC++; T=memory[PC]; if(T&0x80) T-=0x100; 
    if(IR&0x20) {if (ST&branchflag[IR>>6]) {PC+=T;cycles=3;}} else {if (!(ST&branchflag[IR>>6])) {PC+=T;cycles=3;}}  } 
   else {  
    switch (IR&0x1F) { 
     case 0: PC++; addr = PC; cycles=2; break; 
     case 0x1C: PC++; addr=memory[PC]; PC++; addr+=memory[PC]*256 + X; cycles=5; break; 
     case 0xC: PC++; addr=memory[PC]; PC++; addr+=memory[PC]*256; cycles=4; break; 
     case 0x14: PC++; addr = memory[PC] + X; cycles=4; break; 
     case 4: PC++; addr = memory[PC]; cycles=3;  
    }  
    addr&=0xFFFF;  
    switch (IR&0xE0) {
     case 0x00: memory[0x100+SP]=PC%256; SP--;SP&=0xFF; memory[0x100+SP]=PC/256;  SP--;SP&=0xFF; memory[0x100+SP]=ST; SP--;SP&=0xFF; 
       PC = memory[0xFFFE]+memory[0xFFFF]*256-1; cycles=7; break; 
     case 0x20: if(IR&0xF) { ST &= 0x3D; ST |= (memory[addr]&0xC0) | ( !(A&memory[addr]) )<<1; } 
      else { memory[0x100+SP]=(PC+2)%256; SP--;SP&=0xFF; memory[0x100+SP]=(PC+2)/256;  SP--;SP&=0xFF; PC=memory[addr]+memory[addr+1]*256-1; cycles=6; }  break; 
     case 0x40: if(IR&0xF) { PC = addr-1; cycles=3; } 
      else { if(SP>=0xFF) return 0xFE; SP++;SP&=0xFF; ST=memory[0x100+SP]; SP++;SP&=0xFF; T=memory[0x100+SP]; SP++;SP&=0xFF; PC=memory[0x100+SP]+T*256-1; cycles=6; }  break; 
     case 0x60: if(IR&0xF) { PC = memory[addr]+memory[addr+1]*256-1; cycles=5; } 
      else { if(SP>=0xFF) return 0xFF; SP++;SP&=0xFF; T=memory[0x100+SP]; SP++;SP&=0xFF; PC=memory[0x100+SP]+T*256-1; cycles=6; }  break; 
     case 0xC0: T=Y-memory[addr]; ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0); break; 
     case 0xE0: T=X-memory[addr]; ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0); break; 
     case 0xA0: Y=memory[addr]; ST&=125;ST|=(!Y)<<1|(Y&128); break; 
     case 0x80: 
        if (addr >= 0xd400 && addr <= 0xd41c && g_current_frame_pokes) {
             g_current_frame_pokes->push_back({(uint16_t)addr, (uint8_t)Y});
        }
        memory[addr]=Y; storadd=addr;  
    }
   }
  }
 
  PC++; 
  return 0; 
 } 

// Arrays to support the emulation:
unsigned int TriSaw_8580[4096], PulseSaw_8580[4096], PulseTriSaw_8580[4096];

#define PERIOD0 CLOCK_RATIO_DEFAULT 
#define STEP0 3 

float ADSRperiods[16] = {PERIOD0, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251};
byte ADSRstep[16] = {STEP0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

const byte ADSR_exptable[256] = {
  1, 30, 30, 30, 30, 30, 30, 16, 16, 16, 16, 16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

unsigned int combinedWF(char num, char channel, unsigned int* wfarray, int index, char differ6581, byte freqh) {
 static float addf; addf = 0.6+0.4/freqh;
 int n = (int)num;
 int ch = (int)channel;
 if(differ6581 && SID_model[n]==6581) index&=0x7FF; 
 prevwavdata[ch] = wfarray[index]*addf + prevwavdata[ch]*(1.0-addf);
 return prevwavdata[ch];
}

void createCombinedWF(unsigned int* wfarray, float bitmul, float bitstrength, float treshold) {
 int  i,j,k;
 for (i=0; i<4096; i++) { wfarray[i]=0; 
  for (j=0; j<12;j++) {
   float bitlevel=0; 
   for (k=0; k<12; k++) bitlevel += ( bitmul/pow(bitstrength,fabs(k-j)) ) * (((i>>k)&1)-0.5);
   wfarray[i] += (bitlevel>=treshold)? pow(2,j) : 0; } 
  wfarray[i]*=12;  
 }
}

int SID(char num, unsigned int baseaddr) 
{
 static byte channel, ctrl, SR, prevgate, wf, test, *sReg, *vReg;
 static unsigned int accuadd, MSB, pw, wfout;
 static int tmp, step, lim, nonfilt, filtin, filtout, output;
 static float period, steep, rDS_VCR_FET, cutoff[3], resonance[3], ftmp;

 filtin=nonfilt=0; sReg = &memory[baseaddr]; vReg = sReg;

 for (channel = num * SID_CHANNEL_AMOUNT ; channel < (num + 1) * SID_CHANNEL_AMOUNT ; channel++, vReg += 7) {
  ctrl = vReg[4];

  SR = vReg[6]; tmp = 0;
  prevgate = (ADSRstate[channel] & GATE_BITMASK);
  if (prevgate != (ctrl & GATE_BITMASK)) { 
   if (prevgate) ADSRstate[channel] &= 0xFF - (GATE_BITMASK | ATTACK_BITMASK | DECAYSUSTAIN_BITMASK);
   else { 
    ADSRstate[channel] = (GATE_BITMASK | ATTACK_BITMASK | DECAYSUSTAIN_BITMASK); 
    if ((SR & 0xF) > (prevSR[channel] & 0xF)) tmp = 1; 
   }                                                      
  }
  prevSR[channel] = SR; 
  ratecnt[channel] += clock_ratio; if (ratecnt[channel] >= 0x8000) ratecnt[channel] -= 0x8000; 
  if (ADSRstate[channel] & ATTACK_BITMASK) step = vReg[5] >> 4;
  else if (ADSRstate[channel] & DECAYSUSTAIN_BITMASK) step = vReg[5] & 0xF;
  else step = SR & 0xF;
  period = ADSRperiods[step]; step = ADSRstep[step];
  if (ratecnt[channel] >= period && ratecnt[channel] < period + clock_ratio && tmp == 0) { 
   ratecnt[channel] -= period; 
   if ((ADSRstate[channel] & ATTACK_BITMASK) || ++expcnt[channel] == ADSR_exptable[envcnt[channel]]) {
    if (!(ADSRstate[channel] & HOLDZERO_BITMASK)) {
     if (ADSRstate[channel] & ATTACK_BITMASK) 
     { envcnt[channel]+=step; if (envcnt[channel]>=0xFF) {envcnt[channel]=0xFF; ADSRstate[channel] &= 0xFF-ATTACK_BITMASK;} }
     else if ( !(ADSRstate[channel] & DECAYSUSTAIN_BITMASK) || envcnt[channel] > (SR&0xF0) + (SR>>4) )
     { envcnt[channel]-=step; if (envcnt[channel]<=0 && envcnt[channel]+step!=0) {envcnt[channel]=0; ADSRstate[channel]|=HOLDZERO_BITMASK;} }
    }
    expcnt[channel] = 0;
   }
  }
  envcnt[channel] &= 0xFF;
 
  int n = (int)num;
  int ch = (int)channel;
  test = ctrl & TEST_BITMASK;  wf = ctrl & 0xF0;  accuadd = (vReg[0] + vReg[1] * 256) * clock_ratio;
  if (test || ((ctrl & SYNC_BITMASK) && sourceMSBrise[n])) phaseaccu[ch] = 0;
  else { phaseaccu[ch] += accuadd; if (phaseaccu[ch] > 0xFFFFFF) phaseaccu[ch] -= 0x1000000; }
  phaseaccu[ch] &= 0xFFFFFF; MSB = phaseaccu[ch] & 0x800000; sourceMSBrise[n] = (MSB > (prevaccu[ch] & 0x800000)) ? 1 : 0;
  if (wf & NOISE_BITMASK) { 
   tmp = noise_LFSR[ch];
   if (((phaseaccu[ch] & 0x100000) != (prevaccu[ch] & 0x100000)) || accuadd >= 0x100000) 
   { step = (tmp & 0x400000) ^ ((tmp & 0x20000) << 5); tmp = ((tmp << 1) + (step ? 1 : test)) & 0x7FFFFF; noise_LFSR[ch]=tmp; }
   wfout = (wf & 0x70) ? 0 : ((tmp & 0x100000) >> 5) + ((tmp & 0x40000) >> 4) + ((tmp & 0x4000) >> 1) + ((tmp & 0x800) << 1) + ((tmp & 0x200) << 2) + ((tmp & 0x20) << 5) + ((tmp & 0x04) << 7) + ((tmp & 0x01) << 8);
  } 
  else if (wf & PULSE_BITMASK) { 
   pw = (vReg[2] + (vReg[3] & 0xF) * 256) * 16;  tmp = (int) accuadd >> 9;  
   if (0 < (int)pw && (int)pw < tmp) pw = tmp;  
   tmp ^= 0xFFFF;  if ((int)pw > tmp) pw = tmp;  
   tmp = phaseaccu[ch] >> 8;
   if (wf == PULSE_BITMASK) { 
    step = (accuadd>=255)? 65535/(accuadd/256.0) : 0xFFFF; 
    if (test) wfout=0xFFFF;
    else if (tmp<(int)pw) { lim=(0xFFFF-pw)*step; if (lim>0xFFFF) lim=0xFFFF; tmp=lim-(pw-tmp)*step; wfout=(tmp<0)?0:tmp; } 
    else { lim=pw*step; if (lim>0xFFFF) lim=0xFFFF; tmp=(0xFFFF-tmp)*step-lim; wfout=(tmp>=0)?0xFFFF:tmp; } 
   }
   else { 
    wfout = (tmp >= (int)pw || test) ? 0xFFFF:0; 
    if (wf&TRI_BITMASK) { 
     if (wf&SAW_BITMASK) { wfout = wfout? combinedWF(n,ch,PulseTriSaw_8580,tmp>>4,1,vReg[1]) : 0; } 
     else { tmp=phaseaccu[ch]^(ctrl&RING_BITMASK?sourceMSB[n]:0); wfout = (wfout)? combinedWF(n,ch,PulseSaw_8580,(tmp^(tmp&0x800000?0xFFFFFF:0))>>11,0,vReg[1]) : 0; } } 
    else if (wf&SAW_BITMASK) wfout = wfout? combinedWF(n,ch,PulseSaw_8580,tmp>>4,1,vReg[1]) : 0; 
   }
  }
  else if (wf&SAW_BITMASK) { 
   wfout=phaseaccu[channel]>>8; 
   if (wf&TRI_BITMASK) wfout = combinedWF(num,channel,TriSaw_8580,wfout>>4,1,vReg[1]); 
   else { 
    steep=(accuadd/65536.0)/288.0;
    wfout += wfout*steep; if(wfout>0xFFFF) wfout=0xFFFF-(wfout-0x10000)/steep; 
   } 
  }
  else if (wf&TRI_BITMASK) { 
   tmp=phaseaccu[ch]^(ctrl&RING_BITMASK?sourceMSB[n]:0); wfout = (tmp^(tmp&0x800000?0xFFFFFF:0)) >> 7; 
  }
  wfout&=0xFFFF; if (wf) prevwfout[ch] = wfout; else { wfout = prevwfout[ch]; } 
  prevaccu[ch] = phaseaccu[ch]; sourceMSB[n] = MSB;            

  int voice_out_val = ((int)wfout - 0x8000) * envcnt[ch] / 256;
  if (num == 0) g_per_voice_out[(int)ch] = voice_out_val; // ch is 0,1,2 for SID #0
  if (sReg[0x17] & FILTSW[ch]) filtin += voice_out_val;
  else if ((FILTSW[ch] != 4) || !(sReg[0x18] & OFF3_BITMASK)) 
   nonfilt += voice_out_val;
 }
 if((num==0) && (memory[1]&3)) { sReg[0x1B]=wfout>>8; sReg[0x1C]=envcnt[3]; } 

 int n = (int)num;
 cutoff[n] = sReg[0x16] * 8 + (sReg[0x15] & 7);
 if (SID_model[n] == 8580) {
  cutoff[n] = ( 1 - exp((cutoff[n]+2) * cutoff_ratio_8580) ); 
  resonance[n] = ( pow(2, ((4 - (sReg[0x17] >> 4)) / 8.0)) );
 } 
 else { 
  cutoff[n] += round(filtin*FILTER_DISTORTION_6581); 
  rDS_VCR_FET = cutoff[n]<=VCR_FET_TRESHOLD ? 100000000.0 
   : cutoff_steepness_6581/(cutoff[n]-VCR_FET_TRESHOLD); 
  cutoff[n] = ( 1 - exp( cap_6581_reciprocal / (VCR_SHUNT_6581*rDS_VCR_FET/(VCR_SHUNT_6581+rDS_VCR_FET)) / samplerate ) ); 
  resonance[n] = ( (sReg[0x17] > 0x5F) ? 8.0 / (sReg[0x17] >> 4) : 1.41 );
 }  
 filtout=0;
 ftmp = filtin + prevbandpass[n] * resonance[n] + prevlowpass[n];
 if (sReg[0x18] & HIGHPASS_BITMASK) filtout -= ftmp;
 ftmp = prevbandpass[n] - ftmp * cutoff[n];
 prevbandpass[n] = ftmp;
 if (sReg[0x18] & BANDPASS_BITMASK) filtout -= ftmp;
 ftmp = prevlowpass[n] + ftmp * cutoff[n];
 prevlowpass[n] = ftmp;
 if (sReg[0x18] & LOWPASS_BITMASK) filtout += ftmp;    

 output = (nonfilt+filtout) * (sReg[0x18]&0xF) / OUTPUT_SCALEDOWN;
 if (output>=32767) output=32767; else if (output<=-32768) output=-32768; 
 return (int)output; 
}

void initSID() { 
 int i;
 for(i=0xD400;i<=0xD7FF;i++) memory[i]=0; 
 for(i=0xDE00;i<=0xDFFF;i++) memory[i]=0;
 for(i=0;i<9;i++) {ADSRstate[i]=HOLDZERO_BITMASK; ratecnt[i]=envcnt[i]=expcnt[i]=0;} 
}

void cSID_init(int samplerate)
{
  int i;
  clock_ratio = (g_is_pal ? kPAL_CPUCLK : kNTSC_CPUCLK) / samplerate;

  if (clock_ratio > 9) {
    ADSRperiods[0] = clock_ratio;
    ADSRstep[0] = ceil(clock_ratio / 9.0);
  } else {
    ADSRperiods[0] = 9.0;
    ADSRstep[0] = 1;
  }

  cutoff_ratio_8580 = -2 * 3.14 * (12500 / 2048) / samplerate;
  cap_6581_reciprocal = -1000000 / CAP_6581;
  cutoff_steepness_6581 = FILTER_DARKNESS_6581 * (2048.0 - VCR_FET_TRESHOLD);

 createCombinedWF(TriSaw_8580, 0.8, 2.4, 0.64);
 createCombinedWF(PulseSaw_8580, 1.4, 1.9, 0.68);
 createCombinedWF(PulseTriSaw_8580, 0.8, 2.5, 0.64);
    
 for(i = 0; i < 9; i++) {
  ADSRstate[i] = HOLDZERO_BITMASK; envcnt[i] = 0; ratecnt[i] = 0; 
  phaseaccu[i] = 0; prevaccu[i] = 0; expcnt[i] = 0; prevSR[i]=0;
  noise_LFSR[i] = 0x7FFFFF; prevwfout[i] = 0;
 }
 for(i = 0; i < 3; i++) {
  sourceMSBrise[i] = 0; sourceMSB[i] = 0;
  prevlowpass[i] = 0; prevbandpass[i] = 0;
 }
 initSID();
}

void do_init(byte subt)
{
  static int timeout;
  subtune = subt;
  initCPU(initaddr);
  initSID();
  A = subtune;
  memory[1] = 0x37;

/*  memory[0x2a6] = g_is_pal ? 0x1 : 0; // PAL/NTSC flag for the tune's init code, which some tunes check to set up CIA1 timers
  memory[0xDC05] = 0;
  memory[0x314] = 0;
  memory[0x315] = 0;
  memory[0xd011] = 0;
  memory[0xd012] = 0; 
  memory[0xd01a] = 0;
  memory[0xdc0d] = 0;
  memory[0xdc0e] = 0;
  */

  for (timeout = 100000; timeout >= 0; timeout--) {
    if (CPU()) break;
  }

  // Detect kernal-CIA1-timed tunes: playaddf==0 (RSID / no explicit play addr)
  // with the kernal ROM mapped (memory[1]&3 >= 2), meaning playaddr will come
  // from $0314/$0315 rather than the NMI vector.  The kernal sets CIA1 at boot
  // to fire every ~16667 PAL cycles; cSID never sees that write, so we apply it
  // here rather than letting frame_sampleperiod fall back to the VBL rate.
  // Detect kernal-CIA1-timed tunes: playaddf==0 means the SID header has no
  // explicit play address — the tune hooks $0314/$0315 and relies on the kernal's
  // CIA1 timer (pre-initialized at ~60Hz) to drive the IRQ.  cSID never emulates
  // the kernal boot sequence that programs CIA1, so DC05 stays 0 after init and
  // frame_sampleperiod falls back to the VBL rate (50Hz), making the tune slow.
  // Correct condition: playaddf==0 AND timermode not set AND init left CIA1 alone
  // (DC05==0).  The memory[1] HIRAM check was wrong — some tunes disable HIRAM
  // in their init code without that changing the CIA1 rate.

  //g_uses_kernal_cia1 = (playaddf == 0 && !timermode[subtune] && !memory[0xDC05]);
  g_uses_kernal_cia1 = (playaddf == 0 && !timermode[subtune] && !memory[0xDC05] && g_sid_clock_hint == 3); // also require clock hint to be "PAL+NTSC";

  //std::cout << "g_uses_kernal_cia1: " << g_uses_kernal_cia1 << std::endl;
  //std::cout << "0xdc05 after init: " << std::hex << (int)memory[0xDC05] << std::dec << std::endl;
  //std::cout << "0x314/0x315 after init: " << std::hex << (int)memory[0x314] << "/" << (int)memory[0x315] << std::dec << std::endl;
  //std::cout << "0xfffe/0xffff after init: " << std::hex << (int)memory[0xFFFE] << "/" << (int)memory[0xFFFF] << std::dec << std::endl;
  //std::cout << "0xd011 after init: " << std::hex << (int)memory[0xd011] << std::dec << std::endl;
  //std::cout << "0xd012 after init: " << std::hex << (int)memory[0xd012] << std::dec << std::endl;
  //std::cout << "0xd01a after init: " << std::hex << (int)memory[0xd01a] << std::dec << std::endl;
  //std::cout << "0xdc0d after init: " << std::hex << (int)memory[0xdc0d] << std::dec << std::endl;
  //std::cout << "0xdc0e after init: " << std::hex << (int)memory[0xdc0e] << std::dec << std::endl;

  if (timermode[subtune] || memory[0xDC05]) {  
    if (!memory[0xDC05]) {
      memory[0xDC04] = 0x24;
      memory[0xDC05] = 0x40;  
    }
    frame_sampleperiod = (memory[0xDC04] + memory[0xDC05] * 256) / clock_ratio;
    std::cout << "[CIA1] Detected CIA1 timer with period " << frame_sampleperiod << " CPU cycles (" << (samplerate / frame_sampleperiod) << " Hz)" << std::endl;
    g_uses_kernal_cia1 = false; // CIA1 was set by the tune itself — no kFrameSamples override needed
  } else {
    // VBL fallback — also used for kernal-CIA1 tunes; play_frame() scales
    // kFrameSamples to compensate when g_uses_kernal_cia1 is true.
    frame_sampleperiod = samplerate / (g_is_pal ? kPAL_FRAMERATE : kNTSC_FRAMERATE);
    std::cout << "[VBL] No CIA1 timer detected; using VBL rate with frame sample period " << frame_sampleperiod << " CPU cycles (" << (samplerate / frame_sampleperiod) << " Hz)" << std::endl;
  }

  if (playaddf == 0) {
    playaddr = ((memory[1] & 3) < 2) ? memory[0xFFFE] + memory[0xFFFF] * 256 : memory[0x314] + memory[0x315] * 256;
  } else {
    playaddr = playaddf;
    if (playaddr >= 0xE000 && memory[1] == 0x37) {
      memory[1] = 0x35;  
    }
  }

  initCPU(playaddr);
  framecnt = 1;
  finished = 0;
  CPUtime = 0;

  // Reset digi state for this tune. Lazy detection runs in play_frame().
  g_digi_active          = false;
  g_cia2_timer_accum     = 0.0f;
  g_digi_carry_inc_accum = -1.0f;
}

void internal_play(int len);

void internal_init_from_memory(int subtune_idx) {
    int offs = filedata[7];
    int loadaddr = filedata[8] + filedata[9] ? filedata[8] * 256 + filedata[9] : filedata[offs] + filedata[offs + 1] * 256;

    for (int i = 0; i < 32; i++) {
        timermode[31 - i] = (filedata[0x12 + (i >> 3)] & (byte)pow(2, 7 - i % 8)) ? 1 : 0;
    }

    for (int i = 0; i < MAX_DATA_LEN; i++) memory[i] = 0;
    for (int i = offs + 2; i < filedata_len; i++) {
        if (loadaddr + i - (offs + 2) < MAX_DATA_LEN) {
            memory[loadaddr + i - (offs + 2)] = filedata[i];
        }
    }

    initaddr = filedata[0xA] + filedata[0xB] ? filedata[0xA] * 256 + filedata[0xB] : loadaddr;
    playaddr = playaddf = filedata[0xC] * 256 + filedata[0xD];

    SID_address[1] = filedata[0x7A] >= 0x42 && (filedata[0x7A] < 0x80 || filedata[0x7A] >= 0xE0) ? 0xD000 + filedata[0x7A] * 16 : 0;
    SID_address[2] = filedata[0x7B] >= 0x42 && (filedata[0x7B] < 0x80 || filedata[0x7B] >= 0xE0) ? 0xD000 + filedata[0x7B] * 16 : 0;
    SIDamount = 1 + (SID_address[1] > 0) + (SID_address[2] > 0);

    // --- SID header video standard (clock) detection ---
    // The 'flags' WORD is at byte offset +0x76 (big-endian, PSID v2NG and later only).
    // Bits 2-3 of the 16-bit value encode the clock standard:
    //   00 = Unknown, 01 = PAL, 10 = NTSC, 11 = PAL+NTSC
    // Reference: SID_file_format.txt, section "+76 WORD flags".
    {
        uint16_t sid_version = (uint16_t)((filedata[4] << 8) | filedata[5]);
        uint32_t speed = ((uint32_t)filedata[0x12] << 24) | ((uint32_t)filedata[0x13] << 16)
                       | ((uint32_t)filedata[0x14] <<  8) |  (uint32_t)filedata[0x15];
        const bool is_rsid = (filedata[0] == 'R' && filedata[1] == 'S' && filedata[2] == 'I' && filedata[3] == 'D');
        const char* const magic_str = is_rsid ? "RSID" : "PSID";
        const char* const clock_names[] = {"Unknown", "PAL", "NTSC", "PAL+NTSC"};
        if (sid_version >= 2 && filedata_len >= 0x78) {
            uint16_t flags = (uint16_t)((filedata[0x76] << 8) | filedata[0x77]);
            g_sid_clock_hint = (flags >> 2) & 0x03;
            std::cout << "[SID] " << magic_str << " header v" << sid_version
                      << "  clock=" << clock_names[g_sid_clock_hint]
                      << "  flags=0x" << std::hex << std::setw(4) << std::setfill('0') << flags
                      << "  speed=0x" << std::hex << std::setw(8) << std::setfill('0') << speed
                      << "  initaddr=0x" << std::hex << std::setw(4) << std::setfill('0') << initaddr
                      << "  playaddr=0x" << std::hex << std::setw(4) << std::setfill('0') << playaddr
                      << "  timermode=0x" << std::hex << std::setw(2) << std::setfill('0') << timermode[subtune_idx]
                      << std::dec << "\n";
            // When the header names one standard unambiguously, update g_is_pal so all
            // timing (CPU clock, framerate, digi NMI positions) matches the original.
            // Unknown (0) and Both (3) leave g_is_pal at the hardware-supplied value.
            if      (g_sid_clock_hint == 1) g_is_pal = true;   // PAL only
            else if (g_sid_clock_hint == 2) g_is_pal = false;  // NTSC only
        } else {
            g_sid_clock_hint = 0;
            std::cout << "[SID] " << magic_str << " header v" << sid_version
                      << "  speed=0x" << std::hex << std::setw(8) << std::setfill('0') << speed
                      << "  clock=N/A (pre-v2, using hardware default: "
                      << "  timermode=0x" << std::hex << std::setw(2) << std::setfill('0') << timermode[subtune_idx]
                      << (g_is_pal ? "PAL" : "NTSC") << ")\n";
        }
    }

    cSID_init(DEFAULT_SAMPLERATE);
    do_init(subtune_idx); 
}

// Lightweight restart: reload the program image from filedata and re-run do_init
// without rebuilding the wavetables (cSID_init) or zeroing the full 64k memory map.
// This is called on song loop and is ~10x faster than a full internal_init_from_memory.
void internal_restart_from_memory(int subtune_idx) {
    int offs = filedata[7];
    int loadaddr = filedata[8] + filedata[9] ? filedata[8] * 256 + filedata[9] : filedata[offs] + filedata[offs + 1] * 256;

    // Zero only the SID register area and restore the SID program image.
    for (int i = 0xD400; i <= 0xD7FF; i++) memory[i] = 0;
    for (int i = offs + 2; i < filedata_len; i++) {
        if (loadaddr + i - (offs + 2) < MAX_DATA_LEN) {
            memory[loadaddr + i - (offs + 2)] = filedata[i];
        }
    }
    // Re-run the SID init routine (runs the tune's init vector via CPU emulation).
    do_init(subtune_idx);
}

// Like internal_restart_from_memory but skips zeroing the SID register area.
// Use this for seamlessly-looping songs: the play routine keeps SID registers in
// a valid state at the loop boundary, so muting first causes an audible glitch.
void internal_play(int len)  
{
  for (int v = 0; v < 3; v++) g_frame_voice_samples.voice[v].clear();
  g_frame_voice_samples.combined.clear();

  static int i, output;

  for (i = 0; i < len; i += 2) {
    framecnt--;
    if (framecnt <= 0) {
      framecnt = frame_sampleperiod;
      finished = 0;
      PC = playaddr;
      SP = 0xFF;
    }

    if (finished == 0) {
      while (CPUtime <= clock_ratio) {
        pPC = PC;
        if (CPU() >= 0xFE || ((memory[1] & 3) > 1 && pPC < 0xE000 && (PC == 0xEA31 || PC == 0xEA81))) {
          finished = 1;
          break;
        } else {
          CPUtime += cycles; 
        }

        if ((addr == 0xDC05 || addr == 0xDC04) && (memory[1] & 3) && timermode[subtune]) {
          frame_sampleperiod = (memory[0xDC04] + memory[0xDC05] * 256) / clock_ratio;  
          if (!dynCIA) {
            dynCIA = 1;
          }
        }

        if (storadd >= 0xD420 && storadd < 0xD800 && (memory[1] & 3)) {  
          if (!(SID_address[1] <= storadd && storadd < SID_address[1] + 0x1F) &&
              !(SID_address[2] <= storadd && storadd < SID_address[2] + 0x1F)) {
            memory[storadd & 0xD41F] = memory[storadd];  
          }
        }

        if (addr == 0xD404 && !(memory[0xD404] & GATE_BITMASK)) ADSRstate[0] &= 0x3E; 
        if (addr == 0xD40B && !(memory[0xD40B] & GATE_BITMASK)) ADSRstate[1] &= 0x3E;
        if (addr == 0xD412 && !(memory[0xD412] & GATE_BITMASK)) ADSRstate[2] &= 0x3E;
      }
      CPUtime -= clock_ratio;
    }

    output = SID(0, 0xD400);
    // Capture per-voice samples for visualization
    constexpr float kScale = 1.0f / 32768.0f;
    for (int v = 0; v < 3; v++)
      g_frame_voice_samples.voice[v].push_back(g_per_voice_out[v] * kScale);
    if (SIDamount >= 2) output += SID(1, SID_address[1]);
    if (SIDamount == 3) output += SID(2, SID_address[2]);
    g_frame_voice_samples.combined.push_back(output * kScale / std::max(1, SIDamount));
  }
}

} // namespace

const SidVoiceSamples& get_sid_frame_samples() {
    return g_frame_voice_samples;
}

void clear_sid_frame_samples() {
    // Preserve the vector sizes so drawSidWaveforms sees non-empty data
    // (total==0 causes an early-out; we want zero-value samples = flat line).
    int n = (int)g_frame_voice_samples.voice[0].size();
    if (n == 0) n = (int)(DEFAULT_SAMPLERATE / (g_is_pal ? kPAL_FRAMERATE : kNTSC_FRAMERATE));
    for (int v = 0; v < 3; v++)
        g_frame_voice_samples.voice[v].assign(n, 0.0f);
    g_frame_voice_samples.combined.assign(n, 0.0f);
}

// --- SidPlayer Implementation ---

SidPlayer::SidPlayer(bool is_pal) {
    is_pal_machine = is_pal;
    hw_is_pal      = is_pal;
    g_is_pal = is_pal;
}

bool SidPlayer::load(const std::string& path, int subtune) {
    FILE* InputFile = fopen(path.c_str(), "rb");
    if (InputFile == NULL) {
        return false;
    }

    int readata;
    int datalen = 0;
    do {
        readata = fgetc(InputFile);
        filedata[datalen++] = readata;
    } while (readata != EOF && datalen < MAX_DATA_LEN);
    fclose(InputFile);
    datalen--; // Adjust for EOF
    filedata_len = datalen;

    g_is_pal = is_pal_machine;
    internal_init_from_memory(subtune);
    if (speed_compensation_enabled && g_sid_clock_hint != 0 && g_sid_clock_hint != 3 && is_pal_machine != g_is_pal) {
        std::cout << "[SID] clock mismatch: SID requires "
                  << (g_is_pal ? "PAL" : "NTSC") << " but hardware is "
                  << (is_pal_machine ? "PAL" : "NTSC")
                  << " \u2014 using SID-specified timing\n";
        is_pal_machine = g_is_pal;
    }
    play_rate_accum = 1.0f;
    this->current_subtune = subtune;
    this->is_playing = true;
    this->current_frame_pos = 0;

    return true;
}

bool SidPlayer::load_from_memory(const std::vector<uint8_t>& data, int subtune) {
    if (data.size() > MAX_DATA_LEN) {
        std::cerr << "SID data too large\n";
        return false;
    }
    
    memcpy(filedata, data.data(), data.size());
    filedata_len = data.size();

    g_is_pal = is_pal_machine;
    internal_init_from_memory(subtune);
    if (speed_compensation_enabled && g_sid_clock_hint != 0 && g_sid_clock_hint != 3 && is_pal_machine != g_is_pal) {
        std::cout << "[SID] clock mismatch: SID requires "
                  << (g_is_pal ? "PAL" : "NTSC") << " but hardware is "
                  << (is_pal_machine ? "PAL" : "NTSC")
                  << " \u2014 using SID-specified timing\n";
        is_pal_machine = g_is_pal;
    }
    play_rate_accum = 1.0f;

    // Store state for restart
    this->current_path = "MEMORY"; 
    this->current_subtune = subtune;
    this->is_playing = true;
    this->current_frame_pos = 0;

    return true;
}

bool SidPlayer::preload_metadata(const std::vector<uint8_t>& data, int subtune) {
    // Fast synchronous step: copy raw SID bytes so get_title/get_author
    // are immediately usable.  Does NOT run the init vector — call
    // init_from_preloaded() (on a background thread) for the slow part.
    if (data.size() > MAX_DATA_LEN) return false;
    memcpy(filedata, data.data(), data.size());
    filedata_len = (int)data.size();
    this->current_subtune = subtune;
    return true;
}

bool SidPlayer::init_from_preloaded() {
    // Slow step: run cSID_init (wavetable rebuild) + do_init (6502 init vector).
    // Assumes preload_metadata() has already been called.
    g_is_pal = is_pal_machine;
    internal_init_from_memory(this->current_subtune);
    if (speed_compensation_enabled && g_sid_clock_hint != 0 && g_sid_clock_hint != 3 && is_pal_machine != g_is_pal) {
        std::cout << "[SID] clock mismatch: SID requires "
                  << (g_is_pal ? "PAL" : "NTSC") << " but hardware is "
                  << (is_pal_machine ? "PAL" : "NTSC")
                  << " \u2014 using SID-specified timing\n";
        is_pal_machine = g_is_pal;
    }
    play_rate_accum = 1.0f;
    this->current_path = "MEMORY";
    this->is_playing = true;
    this->current_frame_pos = 0;
    return true;
}

bool SidPlayer::init_from_preloaded_unlocked() {
    // Same as init_from_preloaded() but does NOT set is_playing.
    // The caller is responsible for setting is_playing=true under sid_mutex.
    // Guard the anonymous-namespace globals against two concurrent init threads
    // (rapid back-to-back subtune switches).  play_frame never holds this mutex,
    // so it cannot cause render-thread deadline misses.
    std::lock_guard<std::mutex> init_lock(g_sid_init_mutex);
    g_is_pal = is_pal_machine;
    internal_init_from_memory(this->current_subtune);
    if (speed_compensation_enabled && g_sid_clock_hint != 0 && g_sid_clock_hint != 3 && is_pal_machine != g_is_pal) {
        std::cout << "[SID] clock mismatch: SID requires "
                  << (g_is_pal ? "PAL" : "NTSC") << " but hardware is "
                  << (is_pal_machine ? "PAL" : "NTSC")
                  << " \u2014 using SID-specified timing\n";
        is_pal_machine = g_is_pal;
    }
    play_rate_accum = 1.0f;
    this->current_path = "MEMORY";
    this->current_frame_pos = 0;
    return true;
}

float SidPlayer::get_elapsed_seconds() const {
    return (float)current_frame_pos / (is_pal_machine ? kPAL_FRAMERATE : kNTSC_FRAMERATE);
}

float SidPlayer::get_duration_seconds() const {
    if (duration_frames < 0) return -1.0f;
    return (float)duration_frames / (is_pal_machine ? kPAL_FRAMERATE : kNTSC_FRAMERATE);
}

void SidPlayer::set_duration_seconds(float seconds) {
    if (seconds <= 0) {
        duration_frames = -1;
    } else {
        duration_frames = (int)(seconds * (is_pal_machine ? kPAL_FRAMERATE : kNTSC_FRAMERATE));
    }
}

void SidPlayer::set_repeat(int count) {
    initial_repeat_count = count;
    repeat_count = count;
}

void SidPlayer::set_machine_type(bool is_pal) {
    is_pal_machine = is_pal;
    hw_is_pal      = is_pal;
    g_is_pal = is_pal;
}

void SidPlayer::set_volume(uint8_t vol) {
    if (vol > 15) vol = 15;
    master_volume = vol;
}

std::string SidPlayer::get_title() const {
    if (filedata_len < 0x36) return "";
    std::string s(reinterpret_cast<const char*>(&filedata[0x16]), 32);
    auto z = s.find('\0');
    if (z != std::string::npos) s.erase(z);
    return s;
}

std::string SidPlayer::get_author() const {
    if (filedata_len < 0x56) return "";
    std::string s(reinterpret_cast<const char*>(&filedata[0x36]), 32);
    auto z = s.find('\0');
    if (z != std::string::npos) s.erase(z);
    return s;
}

void SidPlayer::restart() {
    g_is_pal = is_pal_machine;
    play_rate_accum = 1.0f;
    internal_restart_from_memory(current_subtune);
    is_playing = true;
    current_frame_pos = 0;
}

void SidPlayer::stop() {
    is_playing = false;
    play_rate_accum = 1.0f;
    // Mute SID
    initSID();
    // Kill digi simulation so a subsequent tune starts clean
    g_digi_active          = false;
    g_cia2_timer_accum     = 0.0f;
    g_digi_carry_inc_accum = -1.0f;
}

void SidPlayer::set_digi_enabled(bool enabled) {
    g_digi_enabled = enabled;
    if (!enabled) {
        g_digi_active          = false;
        g_cia2_timer_accum     = 0.0f;
        g_digi_carry_inc_accum = -1.0f;
    }
}

void SidPlayer::set_speed_compensation(bool enabled) {
    speed_compensation_enabled = enabled;
}

void SidPlayer::play_frame(std::vector<SidRegisterWrite>& out_pokes,
                           std::vector<DigiWrite>* out_digi) {
    if (!is_playing) return;

    // Rate adaptation: when a PAL SID runs on NTSC hardware (play_frame called at
    // ~60 Hz but the tune was written for ~50 Hz), skip ~1 in 6 calls so the
    // effective play rate matches the SID's intended standard.  The fractional
    // accumulator distributes skips evenly rather than bunching them.
    if (hw_is_pal != is_pal_machine) {
        const float sid_fps = is_pal_machine ? kPAL_FRAMERATE : kNTSC_FRAMERATE;
        const float hw_fps  = hw_is_pal      ? kPAL_FRAMERATE : kNTSC_FRAMERATE;
        play_rate_accum += sid_fps / hw_fps;
        if (play_rate_accum < 1.0f)
            return; // skip this hardware frame: SID plays at correct tempo
        play_rate_accum -= 1.0f;
        static bool rate_adapt_logged = false;
        if (!rate_adapt_logged) {
            rate_adapt_logged = true;
            std::cout << "[SID] rate adaptation active: "
                      << (is_pal_machine ? "PAL" : "NTSC") << " SID on "
                      << (hw_is_pal ? "PAL" : "NTSC") << " hardware"
                      << " (ratio=" << (sid_fps / hw_fps) << ")\n";
        }
    }

    g_current_frame_pokes = &out_pokes;
    // kFrameSamples: audio samples to process per engine call.
    // PAL:  samplerate / 50.125 ~= 879 samples; NTSC: samplerate / 59.826 ~= 737 samples.
    //
    // For kernal-CIA1 tunes (play via $0314/$0315 at 60Hz, e.g. Skate or Die),
    // frame_sampleperiod stays at the VBL rate. To fire the play routine at 60Hz
    // we process proportionally MORE samples per engine call:
    //   kFrameSamples = 879 * (60Hz / 50.125Hz) ~= 1052  (PAL)
    // internal_play then fires the routine ~1.197x per call, averaging to 60Hz.
    const float kFramerate  = is_pal_machine ? kPAL_FRAMERATE    : kNTSC_FRAMERATE;
    const float kCpuClk     = is_pal_machine ? kPAL_CPUCLK       : kNTSC_CPUCLK;
    const float kCia1Cycles = is_pal_machine ? kKernalCia1CyclesPAL : kKernalCia1CyclesNTSC;
    int kFrameSamples = (int)(samplerate / kFramerate);
    if (g_uses_kernal_cia1) {
        float cia1_hz = kCpuClk / kCia1Cycles;
        kFrameSamples = (int)(kFrameSamples * (cia1_hz / kFramerate));
        static bool cia1_logged = false;
        if (!cia1_logged) {
            std::cout << "[SID] Kernal-CIA1 timing: kFrameSamples=" << kFrameSamples
                      << " (~" << (int)cia1_hz << " Hz play rate)\n";
            cia1_logged = true;
        }
    }

    internal_play(kFrameSamples * 2);
    g_current_frame_pokes = nullptr;

    // --- CIA2 digi sample simulation ---
    // RSID tunes set up CIA2 inside their IRQ handler rather than in init, so
    // we detect lazily (once per tune, on the first frame after CIA2 is armed).
    if (out_digi && !g_digi_active && g_digi_enabled) {
        detect_cia2_digi();
        if (g_digi_active) {
          /*
            std::cout << "[DigiSID] Detected NMI handler at "
                      << std::hex << g_nmi_addr
                      << " timer period=" << std::dec << g_digi_timer_period
                      << " cycles (~" << (int)((is_pal_machine ? kPAL_CPUCLK : kNTSC_CPUCLK) / g_digi_timer_period)
                      << " Hz)\n";
                      */
        }
    }

    if (out_digi && g_digi_active && g_digi_timer_period > 0) {
        // Re-check the CIA2 timer every frame. If the play routine has stopped
        // the timer ($DD0E bit 0 cleared), the digi section is over — fall
        // through to the normal $D418 path so volume is restored cleanly.
        if (!(memory[0xDD0E] & 0x01)) {
            g_digi_active          = false;
            g_cia2_timer_accum     = 0.0f;
            g_digi_carry_inc_accum = -1.0f;
            //std::cout << "[DigiSID] CIA2 timer stopped — digi playback ended\n";
        }
    }

    if (out_digi && g_digi_active && g_digi_timer_period > 0) {
        // Refresh the timer period in case the handler changed the latch.
        g_digi_timer_period = memory[0xDD04] | (memory[0xDD05] << 8);

        // Remove any $D418 writes from the regular sysop_poke stream — the digi
        // stream owns $D418 this frame.
        out_pokes.erase(
            std::remove_if(out_pokes.begin(), out_pokes.end(),
                           [](const SidRegisterWrite& p){ return p.addr == 0xD418; }),
            out_pokes.end());

        // Pre-simulate each NMI firing for this frame.
        // PAL: 312 lines × 63 cycles = 19 656 cycles/frame.
        // NTSC: 263 lines × 65 cycles = 17 095 cycles/frame.
        const float kFrameCycles = is_pal_machine ? kPAL_FRAME_CYCLES : kNTSC_FRAME_CYCLES;
        const float kLineCycles  = is_pal_machine ? kPAL_LINE_CYCLES  : kNTSC_LINE_CYCLES;
        const int   kTotalLines  = is_pal_machine ? kPAL_TOTAL_LINES  : kNTSC_TOTAL_LINES;

        // For NTSC, rotate all digi line positions forward by the VBlank onset line so
        // the natural inter-NMI gap (which falls on visible lower-border lines 0-12)
        // lands inside VBlank instead.  PAL needs no offset — its gap already falls in
        // VBlank (lines 299+).  The remap is: visual_line = (raw_line + offset) % total.
        // Lines that were near end-of-frame wrap to 0-13 (lower border), which is
        // acceptable — those lines are below the main display area anyway.
        const uint16_t ntsc_digi_offset = is_pal_machine ? 0u : 22u;

        // NTSC: suppress $D020 stripe writes on lines 20-24 (the visible frame-wrap
        // boundary where the bottom and top of the display meet).  $D418 nibble writes
        // are always emitted so audio is unaffected.  PAL: zone is inactive (lo > hi).
        const uint16_t ntsc_supp_lo = is_pal_machine ? 1u : 20u;
        const uint16_t ntsc_supp_hi = is_pal_machine ? 0u : 24u;
        auto in_supp_zone = [&](uint16_t line) -> bool {
          return line >= ntsc_supp_lo && line <= ntsc_supp_hi;
        };

        // Maximum digi CMD_WAIT entries to emit per frame.
        // Each entry becomes a CMD_WAIT+POKE pair in the DMA stream. Too many
        // overwhelms the SYSOP and causes double-length frames (~40ms spikes).
        // run_nmi() is always called so the self-modifying sample pointer advances
        // correctly; we just skip emitting some writes to the DMA stream.
        //constexpr int kMaxDigiPerFrame = 32;
        
        //constexpr int kMaxDigiPerFrame = 16;
        //constexpr int kMaxDigiPerFrame = 64;
        constexpr int kMaxDigiPerFrame = 1000;

        // Temporary sysop_poke capture reused each NMI iteration
        std::vector<SidRegisterWrite> nmi_pokes;

        // Drain any cross-frame INC $D020 carry from the previous frame.
        // When the last NMI of frame N-1 had t_inc >= kFrameCycles, the INC was not
        // clamped — it was saved here so it fires at the correct cycle of this frame.
        if (g_digi_carry_inc_accum >= 0.0f) {
            uint16_t carry_line  = (uint16_t)(g_digi_carry_inc_accum / kLineCycles);
            carry_line = (carry_line + ntsc_digi_offset) % (uint16_t)kTotalLines;
            uint8_t  carry_cycle = (uint8_t)((int)g_digi_carry_inc_accum % (int)kLineCycles) + 1;
            // Don't drop a carried INC that lands in the suppress zone — push it just
            // past the zone so the border is restored quickly without a glitch stripe.
            if (in_supp_zone(carry_line)) { carry_line = ntsc_supp_hi + 1; carry_cycle = 1; }
            out_digi->push_back({ carry_line, carry_cycle, 0x01, 0xD020 }); // carried INC/restore
            g_digi_carry_inc_accum = -1.0f;
        }

        // First pass: count total NMIs this frame so we can compute stride.
        int total_nmis = 0;
        {
            float tt = g_cia2_timer_accum;
            while (true) { tt += (float)g_digi_timer_period; if (tt >= kFrameCycles) break; total_nmis++; }
        }
        // Emit one write every `stride` NMIs (stride=1 means emit all).
        int stride = (total_nmis > kMaxDigiPerFrame) ? (total_nmis / kMaxDigiPerFrame) : 1;

        float t = g_cia2_timer_accum;  // fractional carry from previous frame
        int nmi_index = 0;
        while (true) {
            t += (float)g_digi_timer_period;
            if (t >= kFrameCycles) break;

            nmi_pokes.clear();
            g_current_frame_pokes = &nmi_pokes;
            run_nmi(g_nmi_addr);
            g_current_frame_pokes = nullptr;

            // Always advance state; only emit every `stride`th write to the DMA stream.
            if (nmi_index % stride == 0) {
                for (const auto& p : nmi_pokes) {
                    if (p.addr != 0xD418) continue;
                    // t = CIA2 timer expiry cycle (absolute within frame).
                    // NMI hardware overhead: 7 cycles (push PCH/PCL/P + fetch vector lo/hi
                    //   + fetch first opcode) → T_entry = t + 7.
                    // From T_entry (STA $D418 opcode fetch):
                    //   STA abs  = 4 cycles (opcode+addr_lo+addr_hi+write)  → write at T_entry+3
                    //   DEC abs  = 6 cycles (opcode+addr_lo+addr_hi+R+W+W)  → actual write at T_entry+9
                    //   handler body (nibble read/process) + INC abs (6 cyc) → INC write at T_entry+65
                    // (T_entry+65 is +1 line; occasionally T_entry+66 due to 1-cycle VIC DMA steal)
                    // DMA-bus-trace verified: DEC write = t+16, INC write = t+72; stripe = 56 cycles.
                    float t_dec = t + 16.0f;
                    float t_inc = t + 72.0f;

                    uint16_t dec_line  = (uint16_t)(t_dec / kLineCycles);
                    uint8_t  dec_cycle = (uint8_t)((int)t_dec % (int)kLineCycles) + 1;
                    if (dec_line >= (uint16_t)kTotalLines) dec_line = (uint16_t)(kTotalLines - 1);
                    dec_line = (dec_line + ntsc_digi_offset) % (uint16_t)kTotalLines;

                    // If the INC would wrap past kFrameCycles, carry it into the next frame
                    // rather than clamping to kTotalLines-1 (which inverts the DEC/INC order
                    // on the same line and leaves the border stuck at digi_color).
                    // NOTE: the carry position is stored as raw frame cycles; the NTSC remap
                    // is applied when the carry is drained at the start of the next frame.
                    // Suppress $D020 writes if either end of the stripe lands in the
                    // frame-boundary zone; audio ($D418) is always emitted regardless.
                    const bool dec_in_zone = in_supp_zone(dec_line);

                    if (t_inc >= kFrameCycles) {
                        // Save wrapped position; will be emitted at start of next play_frame.
                        // Store as raw pre-offset cycles; remap is applied at drain time.
                        g_digi_carry_inc_accum = t_inc - kFrameCycles;
                        if (!dec_in_zone)
                            out_digi->push_back({ dec_line, dec_cycle, 0x00, 0xD020 }); // DEC
                        out_digi->push_back({ dec_line, dec_cycle, p.val, 0xD418 }); // nibble
                        // no INC this frame — it will be the first write of the next frame
                    } else {
                        uint16_t inc_line  = (uint16_t)(t_inc / kLineCycles);
                        uint8_t  inc_cycle = (uint8_t)((int)t_inc % (int)kLineCycles) + 1;
                        if (inc_line >= (uint16_t)kTotalLines) inc_line = (uint16_t)(kTotalLines - 1);
                        inc_line = (inc_line + ntsc_digi_offset) % (uint16_t)kTotalLines;
                        const bool inc_in_zone = in_supp_zone(inc_line);
                        const bool suppress_d020 = dec_in_zone || inc_in_zone;
                        if (!suppress_d020)
                            out_digi->push_back({ dec_line, dec_cycle, 0x00, 0xD020 }); // DEC
                        out_digi->push_back({ dec_line, dec_cycle, p.val, 0xD418 }); // nibble
                        if (!suppress_d020)
                            out_digi->push_back({ inc_line, inc_cycle, 0x01, 0xD020 }); // INC
                    }
                }
            }
            nmi_index++;
        }
        g_cia2_timer_accum = t - kFrameCycles;  // carry remainder into next frame

/*
        // Log the digi pokes for debugging:
        for (const auto& d : *out_digi) {
            std::cout << "DigiWrite: line=" << d.raster_line << " cycle=" << (int)d.cycle
                      << " val=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)d.val
                      << " addr=0x" << std::setw(4) << d.addr << std::dec << "\n";
        }
*/

        // Duration accounting uses current_frame_pos below; nothing more to do
        // for digi — skip the normal $D418 volume-scaling block.
    } else {
        // Normal (non-digi) $D418 master volume scaling path.
        bool vol_updated = false;
        for (auto& sysop_poke : out_pokes) {
            if (sysop_poke.addr == 0xD418) {
                uint8_t current_vol = sysop_poke.val & 0x0F;
                uint8_t filter_mode = sysop_poke.val & 0xF0;
                uint8_t new_vol = (uint8_t)((current_vol * master_volume) / 15);
                sysop_poke.val = filter_mode | new_vol;
                vol_updated = true;
            }
        }
        // If master volume is reduced and no volume update happened this frame,
        // force one based on the last known SID state.
        if (!vol_updated && master_volume < 15) {
            uint8_t last_val = memory[0xD418];
            uint8_t current_vol = last_val & 0x0F;
            uint8_t filter_mode = last_val & 0xF0;
            uint8_t new_vol = (uint8_t)((current_vol * master_volume) / 15);
            out_pokes.push_back({0xD418, (uint8_t)(filter_mode | new_vol)});
        }
    }

    // Handle Duration Logic
    current_frame_pos++;
    if (duration_frames != -1) {
        if (current_frame_pos >= duration_frames) {
            // Song finished
            if (repeat_count == -1) {
                current_frame_pos -= duration_frames;
                if (duration_frames <= 250) {
                    restart();  // short tune (<=5s): assume one-shot, reinit to replay
                }
                // long tune: assume seamless loop, just wrap the counter
            } else if (repeat_count > 0) {
                repeat_count--;
                restart();
            } else {
                // Stop
                stop();
                // Also emit a mute command to the output stream to be safe
                // (The stop() call does initSID() but that only clears internal state, 
                // we need to tell the engine to silence the hardware)
                // We can manually push mute pokes here
                for (int i = 0; i < 25; i++) {
                    out_pokes.push_back({(uint16_t)(0xD400 + i), 0});
                }
            }
        }
    }
}
