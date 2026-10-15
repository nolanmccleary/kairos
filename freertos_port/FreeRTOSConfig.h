#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* DE1-SoC Cyclone V, Cortex-A9 MPCore, single core (CPU0). Tuned to match the
 * Kairos environment so the head-to-head runs on identical clock / tick / GIC.
 * See freertos_port/PLAN.md. */

/* ---- scheduler ---- */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_TASK_FPU_SUPPORT              1
#define configCPU_CLOCK_HZ                      ( ( uint32_t ) 800000000 )   /* MPU clock (writeup) */
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )      /* matches gtimer 1 kHz */
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                ( ( uint16_t ) 256 )
#define configMAX_TASK_NAME_LEN                 12
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_TRACE_FACILITY                0
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0

/* ---- memory allocation (heap_4, the allocator under test) ---- */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 4 * 1024 * 1024 ) )  /* heap_4 region, SDRAM */
#define configAPPLICATION_ALLOCATED_HEAP        1    /* we supply ucHeap, placed in SDRAM (freertos_heap_shim.c) */
#define configUSE_MALLOC_FAILED_HOOK            0

/* ---- Cortex-A9 GIC: DE1 PERIPHBASE 0xFFFEC000 (GICC +0x100, GICD +0x1000) ---- */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS         ( 0xFFFED000UL )   /* GICD */
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET ( -0xF00 )         /* GICC 0xFFFEC100 = GICD - 0xF00 */
#define configUNIQUE_INTERRUPT_PRIORITIES               32                 /* A9 GIC: 5 priority bits */
#define configMAX_API_CALL_INTERRUPT_PRIORITY           18

/* ---- optional API ---- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_xTimerPendFunctionCall          0

/* A config mistake should stop loudly under JTAG rather than wander. */
#define configASSERT( x )                       if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ; ; ) {} }

/* Tick-interrupt glue is provided in freertos_glue.c for the scheduler build (step 2).
 * The allocator-only build (step 1) does not compile the port, so these stay undefined. */
#ifdef USE_FREERTOS_SCHED
    extern void vConfigureTickInterrupt( void );
    extern void vClearTickInterrupt( void );
    #define configSETUP_TICK_INTERRUPT()        vConfigureTickInterrupt()
    #define configCLEAR_TICK_INTERRUPT()        vClearTickInterrupt()
#endif

#endif /* FREERTOS_CONFIG_H */
