/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "sysop64.h"

void show_status()
{
    uint32_t sysop_version = sysop_get_version_info();
    printf("FPGA Core: %d.%d.%d.%d\n",
           (sysop_version & 0xFF000000) >> 24,
           (sysop_version & 0x00FF0000) >> 16,
           (sysop_version & 0x0000FF00) >> 8,
           (sysop_version & 0x000000FF));

    uint64_t phi2_ticks = sysop_phi2_counter();
    printf("PHI2 counter: %llu\n", phi2_ticks);

    uint64_t debug_data1 = sysop_debug1();
    printf("DEBUG DATA1: 0x%016" PRIx64 "\n", debug_data1);

    uint64_t debug_data2 = sysop_debug2();
    printf("DEBUG DATA2: 0x%016" PRIx64 "\n", debug_data2);

    uint8_t chl = (debug_data2 >> 49) & 0x7;
    printf("CHL: 0x%x\n", chl);

    uint32_t status1 = sysop_read_status_1();
    // uint8_t port_01 = (status1 >> 24) & 0xFF;
    // printf("Bus monitor $01: %02X\n", port_01);
    uint8_t last_dma_trigger = (uint8_t)((status1 >> 21) & 0x03);
    printf("Last DMA Trigger: %02X, status %08X\n", last_dma_trigger, status1);

    printf("Debug info overlay: ");
    if (status1 & 0x1)
    {
        printf("ON\n");
    }
    else
    {
        printf("OFF\n");
    }
    // TODO: cartridge
    uint8_t cart_type = (uint8_t)((status1 >> 1) & 0xFF);
    printf("Cartridge: ");
    if (cart_type == 0)
        printf("None\n");
    else if (cart_type == 1)
        printf("8KB\n");
    else if (cart_type == 2)
        printf("16KB\n");

    printf("Frame buffer: ");
    uint8_t val = (uint8_t)((status1 >> 9) & 0xFF);
    if (val & 2)
    {
        printf("ON (frame %d)\n", (val & 1));
    }
    else
    {
        printf("OFF\n");
    }

    printf("Custom kernal: ");
    if (status1 & 0x1000)
    {
        printf("ON\n");
    }
    else
    {
        printf("OFF\n");
    }

    printf("\n");
}

static inline long long nsec_diff(struct timespec start, struct timespec end)
{
    return (end.tv_sec - start.tv_sec) * 1000000000LL +
           (end.tv_nsec - start.tv_nsec);
}

void measure_hps_fpga_latency()
{
    struct timespec t1, t2;
    long long ns;

    uint64_t round_trip_time[100];
    uint64_t total = 0;
    uint64_t samples[100];
    for (int i = 0; i < 100; i++)
    {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        samples[i] = sysop_phi2_counter();
        clock_gettime(CLOCK_MONOTONIC, &t2);
        round_trip_time[i] = nsec_diff(t1, t2);
        total += round_trip_time[i];
        printf("Round trip time %lld ns\n", round_trip_time[i]);
    }
    printf("Avg round trip time %lld ns\n", (uint64_t)(total / 100.0));

    for (int i = 0; i < 100; i++)
    {
        samples[i] = sysop_phi2_counter();
    }

    int best = 0;
    int worst = 1000;
    uint64_t last = 0;
    int count = 0;
    int gaps = 0;
    int biggest_gap = 0;
    int biggest_gap_index = 0;
    for (int i = 0; i < 100; i++)
    {
        printf("Sample %d, %llu\n", i, samples[i]);
        if (i == 0)
        {
            last = samples[i];
            count = 1;
        }
        else
        {
            if (samples[i] == last)
                count++;
            else
            {
                uint64_t gap = samples[i] - last;
                if (gap > 1)
                {
                    gaps++;
                    if (gap > biggest_gap)
                    {
                        biggest_gap = gap;
                        biggest_gap_index = i;
                    }
                }
                if (count > best)
                    best = count;

                if (count < worst)
                    worst = count;
                last = samples[i];
                count = 1;
            }
        }
    }
    printf("Fastest: %d, gaps %d, biggest gap %d (sample %d)\n", best, gaps, biggest_gap, biggest_gap_index);
}

static int parse_cmd(const char *arg)
{
    if (strcmp(arg, "--reset")                == 0 || strcmp(arg, "0")  == 0) return 0;
    if (strcmp(arg, "--dma-enable")           == 0 || strcmp(arg, "1")  == 0) return 1;
    if (strcmp(arg, "--dma-disable")          == 0 || strcmp(arg, "2")  == 0) return 2;
    if (strcmp(arg, "--cart-enable")          == 0 || strcmp(arg, "3")  == 0) return 3;
    if (strcmp(arg, "--cart-disable")         == 0 || strcmp(arg, "4")  == 0) return 4;
    if (strcmp(arg, "--nmi-hold")             == 0 || strcmp(arg, "5")  == 0) return 5;
    if (strcmp(arg, "--nmi-release")          == 0 || strcmp(arg, "6")  == 0) return 6;
    if (strcmp(arg, "--io-enable")            == 0 || strcmp(arg, "7")  == 0) return 7;
    if (strcmp(arg, "--io-disable")           == 0 || strcmp(arg, "8")  == 0) return 8;
    if (strcmp(arg, "--nmi-intercept-enable") == 0 || strcmp(arg, "9")  == 0) return 9;
    if (strcmp(arg, "--nmi-intercept-disable")== 0 || strcmp(arg, "10") == 0) return 10;
    if (strcmp(arg, "--sdram-read-test")      == 0 || strcmp(arg, "11") == 0) return 11;
    if (strcmp(arg, "--sdram-write-test")     == 0 || strcmp(arg, "12") == 0) return 12;
    if (strcmp(arg, "--trigger-sampler")      == 0 || strcmp(arg, "13") == 0) return 13;
    if (strcmp(arg, "--fb-enable")            == 0 || strcmp(arg, "14") == 0) return 14;
    if (strcmp(arg, "--fb-disable")           == 0 || strcmp(arg, "15") == 0) return 15;
    if (strcmp(arg, "--fb-flip")              == 0 || strcmp(arg, "16") == 0) return 16;
    if (strcmp(arg, "--cart-16k-enable")      == 0 || strcmp(arg, "17") == 0) return 17;
    if (strcmp(arg, "--kernal-enable")        == 0 || strcmp(arg, "18") == 0) return 18;
    if (strcmp(arg, "--kernal-disable")       == 0 || strcmp(arg, "19") == 0) return 19;
    if (strcmp(arg, "--video-reset")          == 0 || strcmp(arg, "20") == 0) return 20;
    if (strcmp(arg, "--sid-sampler-arm")      == 0 || strcmp(arg, "21") == 0) return 21;
    if (strcmp(arg, "--sid-sampler-stop")     == 0 || strcmp(arg, "22") == 0) return 22;
    if (strcmp(arg, "--debug-overlay-show")   == 0 || strcmp(arg, "23") == 0) return 23;
    if (strcmp(arg, "--debug-overlay-hide")   == 0 || strcmp(arg, "24") == 0) return 24;
    if (strcmp(arg, "--fb-push-back")         == 0 || strcmp(arg, "30") == 0) return 30;
    if (strcmp(arg, "--fb-push-front")        == 0 || strcmp(arg, "31") == 0) return 31;
    if (strcmp(arg, "--busmon-chl-alert-show")== 0 || strcmp(arg, "45") == 0) return 45;
    if (strcmp(arg, "--busmon-chl-alert-hide")== 0 || strcmp(arg, "46") == 0) return 46;
    if (strcmp(arg, "--busmon-chl-override-enable") == 0 || strcmp(arg, "47") == 0) return 47;
    if (strcmp(arg, "--busmon-chl-override-disable")== 0 || strcmp(arg, "48") == 0) return 48;
    if (strcmp(arg, "--vic-debug-sampler-start") == 0 || strcmp(arg, "49") == 0) return 49;
    if (strcmp(arg, "--vic-debug-sampler-stop")  == 0 || strcmp(arg, "50") == 0) return 50;
    if (strcmp(arg, "--sid-alternate") == 0 || strcmp(arg, "51") == 0) return 51;
    if (strcmp(arg, "--sid-default")    == 0 || strcmp(arg, "53") == 0) return 53;
    if (strcmp(arg, "--sid-6581")   == 0 || strcmp(arg, "54") == 0) return 54;
    if (strcmp(arg, "--sid-8580")   == 0 || strcmp(arg, "55") == 0) return 55;
    // archived isevic_debugger: if (strcmp(arg, "--isevic-replay-disable")      == 0 || strcmp(arg, "56") == 0) return 56;

    char *end = NULL;
    long raw_cmd = strtol(arg, &end, 0);
    if (end != arg && *end == '\0' && raw_cmd >= 0 && raw_cmd <= UINT16_MAX)
    {
        return (int)raw_cmd;
    }

    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: cmd <command> [args]\n\n");
        printf("Commands:\n");
        printf("  --reset                       Reset C64\n");
        printf("  --dma-enable                  Own DMA (freeze CPU)\n");
        printf("  --dma-disable                 Release DMA (resume CPU)\n");
        printf("  --cart-enable                 Enable 8K cartridge\n");
        printf("  --cart-disable                Disable cartridge\n");
        printf("  --cart-16k-enable             Enable 16K cartridge\n");
        printf("  --nmi-hold                    Assert NMI\n");
        printf("  --nmi-release                 Release NMI\n");
        printf("  --nmi-intercept-enable        Enable NMI intercept\n");
        printf("  --nmi-intercept-disable       Disable NMI intercept\n");
        printf("  --io-enable                   Enable IO range (DE00-DFFF)\n");
        printf("  --io-disable                  Disable IO range (DE00-DFFF)\n");
        printf("  --sdram-read-test             Test SDRAM read\n");
        printf("  --sdram-write-test            Test SDRAM write\n");
        printf("  --trigger-sampler             Trigger bus sampler\n");
        printf("  --fb-enable                   Enable frame buffer\n");
        printf("  --fb-disable                  Disable frame buffer\n");
        printf("  --fb-flip                     Flip frame buffer\n");
        printf("  --fb-push-back                Push frame buffer to back\n");
        printf("  --fb-push-front               Push frame buffer to front\n");
        printf("  --kernal-enable               Enable kernal replacement\n");
        printf("  --kernal-disable              Disable kernal replacement\n");
        printf("  --video-reset                 Strobe video reset\n");
        printf("  --sid-sampler-arm             Arm SID sampler\n");
        printf("  --sid-sampler-stop            Stop SID sampler\n");
        printf("  --debug-overlay-show          Show debug info overlay\n");
        printf("  --debug-overlay-hide          Hide debug info overlay\n");
        printf("  --busmon-chl-alert-show       Show debug overlay in red on CHL mismatch\n");
        printf("  --busmon-chl-alert-hide       Disable CHL mismatch debug overlay alert\n");
        printf("  --busmon-chl-override-enable  Drive system CHL from inferred $0001\n");
        printf("  --busmon-chl-override-disable Drive system CHL from physical wires\n");
        printf("  --vic-debug-sampler-start     Start VIC debug capture sampler\n");
        printf("  --vic-debug-sampler-stop      Stop VIC debug capture sampler\n");
        printf("  --sid-alternate               Select alternate Sysop-64 SID\n");
        printf("  --sid-default                 Select default SID output\n");
        printf("  --sid-6581                   Set SID model to 6581\n");
        printf("  --sid-8580                   Set SID model to 8580\n");
        // archived isevic_debugger: printf("  --isevic-replay-disable       Return isevic_top to the live C64 bus\n");
        printf("  --measure-latency             Measure HPS<->FPGA latency in C64 cycles\n");
        printf("  --set-nmi-vector <hex-addr>   Set vector to use with NMI intercept\n");
        printf("  --set-sid-volume <0-255>      Set SID volume (HDMI output)\n");
        printf("  s / --status                  Show status\n");
        printf("\nNumeric aliases: 0=reset, 1=dma-enable, 2=dma-disable, 3=cart-enable,\n");
        printf("  4=cart-disable, 5=nmi-hold, 6=nmi-release, 7=io-enable, 8=io-disable,\n");
        printf("  9=nmi-intercept-enable, 10=nmi-intercept-disable, 11=sdram-read-test,\n");
        printf("  12=sdram-write-test, 13=trigger-sampler, 14=fb-enable, 15=fb-disable,\n");
        printf("  16=fb-flip, 17=cart-16k-enable, 18=kernal-enable, 19=kernal-disable,\n");
        printf("  20=video-reset, 21=sid-sampler-arm, 22=sid-sampler-stop,\n");
        printf("  23=debug-overlay-show, 24=debug-overlay-hide, 30=fb-push-back, 31=fb-push-front,\n");
        printf("  45=busmon-chl-alert-show, 46=busmon-chl-alert-hide,\n");
        printf("  47=busmon-chl-override-enable, 48=busmon-chl-override-disable,\n");
        printf("  49=vic-debug-sampler-start, 50=vic-debug-sampler-stop,\n");
        printf("  51=sid-alternate, 53=sid-default, 54=sid-6581, 55=sid-8580\n");
        printf("\n");
        return 0;
    }

    sysop_init();

    const char *arg = argv[1];

    if (strcmp(arg, "s") == 0 || strcmp(arg, "--status") == 0)
    {
        show_status();
    }
    else if (strcmp(arg, "--measure-latency") == 0)
    {
        measure_hps_fpga_latency();
    }
    else if (strcmp(arg, "--set-nmi-vector") == 0)
    {
        uint16_t addr = (uint16_t)strtoll(argv[2], NULL, 16);
        printf("Set NMI vector to %04X\n", addr);
        sysop_set_nmi_vector(addr);
    }
    else if (strcmp(arg, "--set-sid-volume") == 0)
    {
        int volume = atoi(argv[2]);
        if (volume < 0 || volume > 255)
        {
            printf("Volume must be between 0 and 255\n");
        }
        else
        {
            printf("Setting SID volume to %d\n", volume);
            sysop_audio_set_sid_volume(volume, volume);
        }
    }
    else
    {
        int cmd = parse_cmd(arg);
        if (cmd < 0)
        {
            printf("Unknown command: %s\n", arg);
            printf("Run 'cmd' with no arguments for usage.\n");
        }
        else if (cmd == 20)
        {
            printf("Strobing video reset\n");
            sysop_video_reset();
            show_status();
        }
        else
        {
            printf("Strobing command %d\n", cmd);
            sysop_command((uint16_t)cmd);
            show_status();
        }
    }

    sysop_uninit();
    return 0;
}
