/* 
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-12     Bernard      first version
 * 2006-05-27     Bernard      add support for same priority thread schedule
 * 2006-08-10     Bernard      remove the last rt_schedule in rt_tick_increase
 * 2010-03-08     Bernard      remove rt_passed_second
 * 2010-05-20     Bernard      fix the tick exceeds the maximum limits
 * 2010-07-13     Bernard      fix rt_tick_from_millisecond issue found by kuronca
 * 2011-06-26     Bernard      add rt_tick_set function.
 * 2018-11-22     Jesven       add per cpu tick
 * 2020-12-29     Meco Man     implement rt_tick_get_millisecond()
 * 2021-06-01     Meco Man     add critical section projection for rt_tick_increase()
 */

#include <rthw.h>
#include <rtthread.h>

// 时钟节拍的长度可以根据 RT_TICK_PER_SECOND 的定义来调整，等于 1/RT_TICK_PER_SECOND 秒
#ifdef RT_USING_SMP // 对称多处理器
#define rt_tick rt_cpu_index(0)->tick
#else
static volatile rt_tick_t rt_tick = 0;
#endif /* RT_USING_SMP */

#ifndef __on_rt_tick_hook
    #define __on_rt_tick_hook()          __ON_HOOK_ARGS(rt_tick_hook, ())
#endif

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_tick_hook)(void);

/**
 * @addtogroup Hook
 */

/**@{*/

/**
 * This function will set a hook function, which will be invoked when tick increase
 *
 *
 * @param hook the hook function
 */
void rt_tick_sethook(void (*hook)(void))
{
    rt_tick_hook = hook;
}
/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup Clock
 */

/**@{*/

/**
 * @brief    This function will return current tick from operating system startup.
 *
 * @return   Return current tick.
 */
rt_tick_t rt_tick_get(void) // 获取时钟节拍
{
    /* return the global tick */
    return rt_tick;
}
RTM_EXPORT(rt_tick_get);

/**
 * @brief    This function will set current tick.
 *
 * @param    tick is the value that you will set.
 */
void rt_tick_set(rt_tick_t tick) // 设置时钟节拍
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    rt_tick = tick;
    rt_hw_interrupt_enable(level);
}

/**
 * @brief    This function will notify kernel there is one tick passed.
 *           Normally, this function is invoked by clock ISR.
 */
// 在中断函数中调用 rt_tick_increase() 对全局变量 rt_tick 进行自加
void rt_tick_increase(void)
{
    struct rt_thread *thread;
    rt_base_t level;

    RT_OBJECT_HOOK_CALL(rt_tick_hook, ());

    level = rt_hw_interrupt_disable();

   // 全局变量 rt_tick 自加
#ifdef RT_USING_SMP
    rt_cpu_self()->tick ++;
#else
    ++ rt_tick;
#endif /* RT_USING_SMP */

    /* 检查时间片 */

    // 每经过一个时钟节拍时，都会检查当前线程的时间片是否用完，以及是否有定时器超时
    thread = rt_thread_self();

    -- thread->remaining_tick;
    if (thread->remaining_tick == 0)
    {
        /* change to initialized tick */
        thread->remaining_tick = thread->init_tick; // 重新赋初值
        thread->stat |= RT_THREAD_STAT_YIELD; // 线程挂起

        rt_hw_interrupt_enable(level);
        rt_schedule();
    }
    else
    {
        rt_hw_interrupt_enable(level);
    }

    rt_timer_check(); // 检查系统硬件定时器链表，如果有定时器超时，将调用相应的超时函数
}

/**
 * @brief    This function will calculate the tick from millisecond.
 *
 * @param    ms is the specified millisecond.
 *              - Negative Number wait forever
 *              - Zero not wait
 *              - Max 0x7fffffff
 *
 * @return   Return the calculated tick.
 */
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms) // 用毫秒值设置 tick
{
    rt_tick_t tick;

    if (ms < 0) // < 0 当成无限大
    {
        tick = (rt_tick_t)RT_WAITING_FOREVER;
    }
    else
    {
        tick = RT_TICK_PER_SECOND * (ms / 1000); // 秒对应 tick
        tick += (RT_TICK_PER_SECOND * (ms % 1000) + 999) / 1000; // 余数(<1s)对应的 tick
    }

    /* return the calculated tick */
    return tick;
}
RTM_EXPORT(rt_tick_from_millisecond);

/**
 * @brief    This function will return the passed millisecond from boot.
 *
 * @note     if the value of RT_TICK_PER_SECOND is lower than 1000 or
 *           is not an integral multiple of 1000, this function will not
 *           provide the correct 1ms-based tick.
 *
 * @return   Return passed millisecond from boot.
 */
RT_WEAK rt_tick_t rt_tick_get_millisecond(void) // 获取当前 tick 对应毫秒值
{
#if 1000 % RT_TICK_PER_SECOND == 0u // RT_TICK_PER_SECOND 必须小于 1000 且是 1000 的因数
    return rt_tick_get() * (1000u / RT_TICK_PER_SECOND);
#else
    #warning "rt-thread cannot provide a correct 1ms-based tick any longer,\
    please redefine this function in another file by using a high-precision hard-timer."
    return 0;
#endif /* 1000 % RT_TICK_PER_SECOND == 0u */
}

/**@}*/

