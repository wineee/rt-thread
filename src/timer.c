/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-12     Bernard      first version
 * 2006-04-29     Bernard      implement thread timer
 * 2006-06-04     Bernard      implement rt_timer_control
 * 2006-08-10     Bernard      fix the periodic timer bug
 * 2006-09-03     Bernard      implement rt_timer_detach
 * 2009-11-11     LiJin        add soft timer
 * 2010-05-12     Bernard      fix the timer check bug.
 * 2010-11-02     Charlie      re-implement tick overflow issue
 * 2012-12-15     Bernard      fix the next timeout issue in soft timer
 * 2014-07-12     Bernard      does not lock scheduler when invoking soft-timer
 *                             timeout function.
 * 2021-08-15     supperthomas add the comment
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to timer.c
 */

/*
 *定时器有硬件定时器和软件定时器之分：
 *1）硬件定时器是芯片本身提供的定时功能。一般是由外部晶振提供给芯片输入时钟，芯片向软件模块提供一组配置寄存器，接受控制输入，到达设定时间值后芯片中断控制器产生时钟中断。硬件定时器的精度一般很高，可以达到纳秒级别，并且是中断触发方式。
 *2）软件定时器是由操作系统提供的一类系统接口，它构建在硬件定时器基础之上，使系统能够提供不受数目限制的定时器服务。
 *
 * RT-Thread 的定时器提供两类定时器机制：第一类是单次触发定时器，这类定时器在启动后只会触发一次定时器事件，然后定时器自动停止。第二类是周期触发定时器，这类定时器会周期性的触发定时器事件，直到用户手动的停止，否则将永远持续执行下去。
 *
 *HARD_TIMER 模式的定时器超时函数在中断上下文环境中执行，可以在初始化 / 创建定时器时使用参数 RT_TIMER_FLAG_HARD_TIMER 来指定。
 *SOFT_TIMER 模式被启用后，系统会在初始化时创建一个 timer 线程，然后 SOFT_TIMER 模式的定时器超时函数在都会在 timer 线程的上下文环境中执行
 * */

#include <rtthread.h>
#include <rthw.h>

/* hard timer list */
static rt_list_t _timer_list[RT_TIMER_SKIP_LIST_LEVEL]; 
// 跳表, 定时器按 tick 从小到大排序，使用跳表可以快速插入, 删除,查找

#ifdef RT_USING_TIMER_SOFT

#define RT_SOFT_TIMER_IDLE              1
#define RT_SOFT_TIMER_BUSY              0

#ifndef RT_TIMER_THREAD_STACK_SIZE
#define RT_TIMER_THREAD_STACK_SIZE     512
#endif /* RT_TIMER_THREAD_STACK_SIZE */

#ifndef RT_TIMER_THREAD_PRIO
#define RT_TIMER_THREAD_PRIO           0
#endif /* RT_TIMER_THREAD_PRIO */

/* soft timer status */
static rt_uint8_t _soft_timer_status = RT_SOFT_TIMER_IDLE;
/* soft timer list */
static rt_list_t _soft_timer_list[RT_TIMER_SKIP_LIST_LEVEL];
static struct rt_thread _timer_thread; // 时间管理线程
ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t _timer_thread_stack[RT_TIMER_THREAD_STACK_SIZE];
#endif /* RT_USING_TIMER_SOFT */

#ifndef __on_rt_object_take_hook
    #define __on_rt_object_take_hook(parent)        __ON_HOOK_ARGS(rt_object_take_hook, (parent))
#endif
#ifndef __on_rt_object_put_hook
    #define __on_rt_object_put_hook(parent)         __ON_HOOK_ARGS(rt_object_put_hook, (parent))
#endif
#ifndef __on_rt_timer_enter_hook
    #define __on_rt_timer_enter_hook(t)             __ON_HOOK_ARGS(rt_timer_enter_hook, (t))
#endif
#ifndef __on_rt_timer_exit_hook
    #define __on_rt_timer_exit_hook(t)              __ON_HOOK_ARGS(rt_timer_exit_hook, (t))
#endif

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
extern void (*rt_object_take_hook)(struct rt_object *object);
extern void (*rt_object_put_hook)(struct rt_object *object);
static void (*rt_timer_enter_hook)(struct rt_timer *timer);
static void (*rt_timer_exit_hook)(struct rt_timer *timer);

/**
 * @addtogroup Hook
 */

/**@{*/

/**
 * @brief This function will set a hook function on timer,
 *        which will be invoked when enter timer timeout callback function.
 *
 * @param hook is the function point of timer
 */
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer))
{
    rt_timer_enter_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be
 *        invoked when exit timer timeout callback function.
 *
 * @param hook is the function point of timer
 */
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer))
{
    rt_timer_exit_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */


/**
 * @brief [internal] The init funtion of timer
 *
 *        The internal called function of rt_timer_init
 *
 * @see rt_timer_init
 *
 * @param timer is timer object
 *
 * @param timeout is the timeout function
 *
 * @param parameter is the parameter of timeout function
 *
 * @param time is the tick of timer
 *
 * @param flag the flag of timer
 */
static void _timer_init(rt_timer_t timer,			// 定时器句柄，指向要初始化的定时器控制块

                           void (*timeout)(void *parameter),	// 超时后需要执行的函数
                           void      *parameter,		// 上述函数的参数
                           rt_tick_t  time,			// 
                           rt_uint8_t flag)
{
    int i;

    /* set flag */
    timer->parent.flag  = flag;

    /* set deactivated */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    timer->timeout_func = timeout;
    timer->parameter    = parameter;

    timer->timeout_tick = 0;
    timer->init_tick    = time;

    /* initialize timer list */
    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        rt_list_init(&(timer->row[i]));
    }
}

/**
 * @brief  Find the next emtpy timer ticks
 *
 * @param timer_list is the array of time list
 *
 * @param timeout_tick is the next timer's ticks
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 */
static rt_err_t _timer_list_next_timeout(rt_list_t timer_list[], rt_tick_t *timeout_tick)
{
    struct rt_timer *timer;
    register rt_base_t level;

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    if (!rt_list_isempty(&timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1])) // 非空
    {
        timer = rt_list_entry(timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                              struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        *timeout_tick = timer->timeout_tick;

        /* enable interrupt */
        rt_hw_interrupt_enable(level);

        return RT_EOK;
    }

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    return -RT_ERROR; // 异常返回的是负数
}

/**
 * @brief Remove the timer
 *
 * @param timer the point of the timer
 */
rt_inline void _timer_remove(rt_timer_t timer)
{
    int i;

    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++) // 从跳表中删除
    {
        rt_list_remove(&timer->row[i]);
    }
}

#if RT_DEBUG_TIMER
/**
 * @brief The number of timer
 *
 * @param timer the head of timer
 *
 * @return count of timer
 */
static int _timer_count_height(struct rt_timer *timer) // 返回跳表节点高度
{
    int i, cnt = 0;

    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        if (!rt_list_isempty(&timer->row[i]))
            cnt++;
    }
    return cnt;
}
/**
 * @brief dump the all timer information
 *
 * @param timer_heads the head of timer
 */
void rt_timer_dump(rt_list_t timer_heads[])
{
    rt_list_t *list;

    for (list = timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1].next;
         list != &timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1];
         list = list->next) // 遍历跳表（环形的）
    {
        struct rt_timer *timer = rt_list_entry(list,
                                               struct rt_timer,
                                               row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        rt_kprintf("%d", _timer_count_height(timer)); // 输出结点高度
    }
    rt_kprintf("\n");
}
#endif /* RT_DEBUG_TIMER */

/**
 * @addtogroup Clock
 */

/**@{*/

/**
 * @brief This function will initialize a timer
 *        normally this function is used to initialize a static timer object.
 *
 * @param timer is the point of timer
 *
 * @param name is a pointer to the name of the timer
 *
 * @param timeout is the callback of timer
 *
 * @param parameter is the param of the callback
 *
 * @param time is timeout ticks of timer
 *
 *             NOTE: The max timeout tick should be no more than (RT_TICK_MAX/2 - 1).
 *
 * @param flag is the flag of timer
 *
 */
void rt_timer_init(rt_timer_t  timer, // 定时器句柄，指向要初始化的定时器控制块
                   const char *name, // 定时器的名称
                   void (*timeout)(void *parameter), // 定时器超时函数指针
                   void       *parameter, // 超时函数的入口参数
                   rt_tick_t   time,  // 定时器的超时时间，单位是时钟节拍
                   rt_uint8_t  flag) // 定时器创建时的参数
{
    /* parameter check */
    RT_ASSERT(timer != RT_NULL); // 一般是静态分配好的，rt_timer_create 是动态申请
    RT_ASSERT(timeout != RT_NULL);
    RT_ASSERT(time < RT_TICK_MAX / 2); // TODO why / 2  

    /* timer object initialization */
    rt_object_init(&(timer->parent), RT_Object_Class_Timer, name);

    _timer_init(timer, timeout, parameter, time, flag);
}
RTM_EXPORT(rt_timer_init);

/**
 * @brief This function will detach a timer from timer management.
 *
 * @param timer is the timer to be detached
 *
 * @return the status of detach
 */

// 当一个静态定时器不需要再使用时，可以使用下面的函数接口
rt_err_t rt_timer_detach(rt_timer_t timer)
{
    register rt_base_t level;

    /* parameter check */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    RT_ASSERT(rt_object_is_systemobject(&timer->parent));

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    _timer_remove(timer);
    /* stop timer */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    rt_object_detach(&(timer->parent));
    // 把定时器对象从内核对象容器中脱离
    // 但是定时器对象所占有的内存不会被释放
    return RT_EOK;
}
RTM_EXPORT(rt_timer_detach);

#ifdef RT_USING_HEAP
/**
 * @brief This function will create a timer
 *
 * @param name is the name of timer
 *
 * @param timeout is the timeout function
 *
 * @param parameter is the parameter of timeout function
 *
 * @param time is timeout ticks of the timer
 *
 *             NOTE: The max timeout tick should be no more than (RT_TICK_MAX/2 - 1).
 *
 * @param flag is the flag of timer
 *
 * @return the created timer object
 */
rt_timer_t rt_timer_create(const char *name,	// 定时器的名称
                           void (*timeout)(void *parameter), // 超时函数指针
                           void       *parameter, // 超时函数的入口参数
                           rt_tick_t   time, // 超时时间，单位是时钟节拍
                           rt_uint8_t  flag) // 定时器创建时的参数，支持的值包括单次定时、周期定时、硬件定时器、软件定时器等
{
    struct rt_timer *timer;

    /* parameter check */
    RT_ASSERT(timeout != RT_NULL);
    RT_ASSERT(time < RT_TICK_MAX / 2);

    /* allocate a object */
    timer = (struct rt_timer *)rt_object_allocate(RT_Object_Class_Timer, name); // 从堆中分配定时器控制块
    if (timer == RT_NULL)
    {
        return RT_NULL; // 创建失败
    }

    _timer_init(timer, timeout, parameter, time, flag); // 对该控制块进行基本的初始化

    return timer; // 创建成功, 返回定时器的句柄
}
RTM_EXPORT(rt_timer_create);

/**
 * @brief This function will delete a timer and release timer memory
 *
 * @param 定时器句柄，指向要删除的定时器
 *
 * @return 删除成功（如果参数 timer 句柄是一个 RT_NULL，将会导致一个 ASSERT 断言）
 */
rt_err_t rt_timer_delete(rt_timer_t timer)
{
    register rt_base_t level;

    /* parameter check */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    RT_ASSERT(rt_object_is_systemobject(&timer->parent) == RT_FALSE);

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    _timer_remove(timer); // 把这个定时器从 rt_timer_list 链表中删除
    /* stop timer */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED; // ACTIVATED 位 flag 置 0

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    rt_object_delete(&(timer->parent)); // 释放相应的定时器控制块占有的内存

    return RT_EOK;
}
RTM_EXPORT(rt_timer_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief This function will start the timer
 *
 * @param timer the timer to be started
 *
 * @return the operation status, RT_EOK on OK, -RT_ERROR on error
 */
rt_err_t rt_timer_start(rt_timer_t timer) // 定时器启动函数
{
    unsigned int row_lvl;
    rt_list_t *timer_list;
    register rt_base_t level;
    register rt_bool_t need_schedule;
    rt_list_t *row_head[RT_TIMER_SKIP_LIST_LEVEL];
    unsigned int tst_nr;
    static unsigned int random_nr;

    /* parameter check */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    need_schedule = RT_FALSE;

    /* stop timer firstly */
    level = rt_hw_interrupt_disable();
    /* remove timer from list */
    _timer_remove(timer); // 从跳表中删除
    /* change status of timer */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED; // 标记不再活跃

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(timer->parent))); // 调用钩子函数

    timer->timeout_tick = rt_tick_get() + timer->init_tick; // 下一次调用时的 tick

#ifdef RT_USING_TIMER_SOFT
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        /* insert timer to soft timer list */
	// SOFT 模式被启用后，系统会在初始化时创建一个 timer 线程，然后 SOFT_TIMER 模式的定时器超时函数在都会在 timer 线程的上下文环境中执行
        timer_list = _soft_timer_list;
    }
    else
#endif /* RT_USING_TIMER_SOFT */
    {
        /* insert timer to system timer list */
        timer_list = _timer_list;
    }

    // 按照超时顺序插入到 rt_timer_list 队列链表
    row_head[0]  = &timer_list[0]; // rt_list_t row_head[]
    for (row_lvl = 0; row_lvl < RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        for (; row_head[row_lvl] != timer_list[row_lvl].prev;
             row_head[row_lvl]  = row_head[row_lvl]->next)
        {
            struct rt_timer *t;
            rt_list_t *p = row_head[row_lvl]->next;

            /* fix up the entry pointer */
            t = rt_list_entry(p, struct rt_timer, row[row_lvl]);

            /* If we have two timers that timeout at the same time, it's
             * preferred that the timer inserted early get called early.
             * So insert the new timer to the end the the some-timeout timer
             * list.
             */
            if ((t->timeout_tick - timer->timeout_tick) == 0)
            {
                continue; // 可能多个函数同时超时
            }
            else if ((t->timeout_tick - timer->timeout_tick) < RT_TICK_MAX / 2)
            {
                break;
            }
        }
        if (row_lvl != RT_TIMER_SKIP_LIST_LEVEL - 1) // 
            row_head[row_lvl + 1] = row_head[row_lvl] + 1;
    }

    /* Interestingly, this super simple timer insert counter works very very
     * well on distributing the list height uniformly. By means of "very very
     * well", I mean it beats the randomness of timer->timeout_tick very easily
     * (actually, the timeout_tick is not random and easy to be attacked). */
    random_nr++;
    tst_nr = random_nr;

    rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - 1],
                         &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
    for (row_lvl = 2; row_lvl <= RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        if (!(tst_nr & RT_TIMER_SKIP_LIST_MASK))
            rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - row_lvl],
                                 &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - row_lvl]));
        else
            break;
        /* Shift over the bits we have tested. Works well with 1 bit and 2
         * bits. */
        tst_nr >>= (RT_TIMER_SKIP_LIST_MASK + 1) >> 1;
    }

    timer->parent.flag |= RT_TIMER_FLAG_ACTIVATED;

#ifdef RT_USING_TIMER_SOFT
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        /* check whether timer thread is ready */
        if ((_soft_timer_status == RT_SOFT_TIMER_IDLE) &&
           ((_timer_thread.stat & RT_THREAD_STAT_MASK) == RT_THREAD_SUSPEND))
        {
            /* resume timer thread to check soft timer */
            rt_thread_resume(&_timer_thread);
            need_schedule = RT_TRUE;
        }
    }
#endif /* RT_USING_TIMER_SOFT */

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    if (need_schedule)
    {
        rt_schedule();
    }

    return RT_EOK;
}
RTM_EXPORT(rt_timer_start);

/**
 * @brief This function will stop the timer
 *
 * @param timer the timer to be stopped
 *
 * @return the operation status, RT_EOK on OK, -RT_ERROR on error
 */
rt_err_t rt_timer_stop(rt_timer_t timer)
{
    register rt_base_t level;

    /* parameter check */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    if (!(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)) // 本来就是停止的
        return -RT_ERROR;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(timer->parent)));

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    _timer_remove(timer);
    /* change status */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}
RTM_EXPORT(rt_timer_stop);

/**
 * @brief This function will get or set some options of the timer
 *
 * @param timer the timer to be get or set
 * @param cmd the control command
 * @param arg the argument
 *
 * @return the statu of control
 */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg) 
{
    // cmd : 用于控制定时器的命令，当前支持四个命令，分别是设置定时时间，查看定时时间，设置单次触发，设置周期触发
    // arg: 与 cmd 相对应的控制命令参数 比如，cmd 为设定超时时间时，就可以将超时时间参数通过 arg 进行设定

    register rt_base_t level;

    /* parameter check */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    level = rt_hw_interrupt_disable();
    switch (cmd)
    {
    case RT_TIMER_CTRL_GET_TIME:
        *(rt_tick_t *)arg = timer->init_tick;
        break;

    case RT_TIMER_CTRL_SET_TIME:
        RT_ASSERT((*(rt_tick_t *)arg) < RT_TICK_MAX / 2);
        timer->init_tick = *(rt_tick_t *)arg;
        break;

    case RT_TIMER_CTRL_SET_ONESHOT: // PERIODIC 位置 0， 定时器只执行一次
        timer->parent.flag &= ~RT_TIMER_FLAG_PERIODIC;
        break;

    case RT_TIMER_CTRL_SET_PERIODIC: // PERIODIC 位置 1
        timer->parent.flag |= RT_TIMER_FLAG_PERIODIC;
        break;

    case RT_TIMER_CTRL_GET_STATE: // 是否 ACTIVAT
        if(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
        {
            /*timer is start and run*/
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_ACTIVATED;
        }
        else
        {
            /*timer is stop*/
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_DEACTIVATED;
        }
    case RT_TIMER_CTRL_GET_REMAIN_TIME:
        *(rt_tick_t *)arg =  timer->timeout_tick;
        break;

    default:
        break;
    }
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}
RTM_EXPORT(rt_timer_control);

/**
 * @brief This function will check timer list, if a timeout event happens,
 *        the corresponding timeout function will be invoked.
 *
 * @note This function shall be invoked in operating system timer interrupt.
 */
void rt_timer_check(void)
{
    struct rt_timer *t;
    rt_tick_t current_tick;
    register rt_base_t level;
    rt_list_t list;

    rt_list_init(&list);

    RT_DEBUG_LOG(RT_DEBUG_TIMER, ("timer check enter\n"));

    current_tick = rt_tick_get();

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    while (!rt_list_isempty(&_timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        t = rt_list_entry(_timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                          struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);

        /*
         * It supposes that the new tick shall less than the half duration of
         * tick max.
         */
        if ((current_tick - t->timeout_tick) < RT_TICK_MAX / 2)
        {
            RT_OBJECT_HOOK_CALL(rt_timer_enter_hook, (t));

            /* remove timer from timer list firstly */
            _timer_remove(t);
            if (!(t->parent.flag & RT_TIMER_FLAG_PERIODIC))
            {
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
            }
            /* add timer to temporary list  */
            rt_list_insert_after(&list, &(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
            /* call timeout function */
            t->timeout_func(t->parameter);

            /* re-get tick */
            current_tick = rt_tick_get();

            RT_OBJECT_HOOK_CALL(rt_timer_exit_hook, (t));
            RT_DEBUG_LOG(RT_DEBUG_TIMER, ("current tick: %d\n", current_tick));

            /* Check whether the timer object is detached or started again */
            if (rt_list_isempty(&list))
            {
                continue;
            }
            rt_list_remove(&(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
            if ((t->parent.flag & RT_TIMER_FLAG_PERIODIC) &&
                (t->parent.flag & RT_TIMER_FLAG_ACTIVATED))
            {
                /* start it */
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
                rt_timer_start(t);
            }
        }
        else break;
    }

    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    RT_DEBUG_LOG(RT_DEBUG_TIMER, ("timer check leave\n"));
}

/**
 * @brief This function will return the next timeout tick in the system.
 *
 * @return the next timeout tick in the system
 */
rt_tick_t rt_timer_next_timeout_tick(void)
{
    rt_tick_t next_timeout = RT_TICK_MAX;
    _timer_list_next_timeout(_timer_list, &next_timeout);
    return next_timeout;
}

#ifdef RT_USING_TIMER_SOFT
/**
 * @brief This function will check software-timer list, if a timeout event happens, the
 *        corresponding timeout function will be invoked.
 */
void rt_soft_timer_check(void)
{
    rt_tick_t current_tick;
    struct rt_timer *t;
    register rt_base_t level;
    rt_list_t list;

    rt_list_init(&list);

    RT_DEBUG_LOG(RT_DEBUG_TIMER, ("software timer check enter\n"));

    /* disable interrupt */
    level = rt_hw_interrupt_disable();

    while (!rt_list_isempty(&_soft_timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        t = rt_list_entry(_soft_timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                            struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);

        current_tick = rt_tick_get();

        /*
         * It supposes that the new tick shall less than the half duration of
         * tick max.
         */
        if ((current_tick - t->timeout_tick) < RT_TICK_MAX / 2)
        {
            RT_OBJECT_HOOK_CALL(rt_timer_enter_hook, (t));

            /* remove timer from timer list firstly */
            _timer_remove(t);
            if (!(t->parent.flag & RT_TIMER_FLAG_PERIODIC))
            {
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
            }
            /* add timer to temporary list  */
            rt_list_insert_after(&list, &(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));

            _soft_timer_status = RT_SOFT_TIMER_BUSY;
            /* enable interrupt */
            rt_hw_interrupt_enable(level);

            /* call timeout function */
            t->timeout_func(t->parameter);

            RT_OBJECT_HOOK_CALL(rt_timer_exit_hook, (t));
            RT_DEBUG_LOG(RT_DEBUG_TIMER, ("current tick: %d\n", current_tick));

            /* disable interrupt */
            level = rt_hw_interrupt_disable();

            _soft_timer_status = RT_SOFT_TIMER_IDLE;
            /* Check whether the timer object is detached or started again */
            if (rt_list_isempty(&list))
            {
                continue;
            }
            rt_list_remove(&(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
            if ((t->parent.flag & RT_TIMER_FLAG_PERIODIC) &&
                (t->parent.flag & RT_TIMER_FLAG_ACTIVATED))
            {
                /* start it */
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
                rt_timer_start(t);
            }
        }
        else break; /* not check anymore */
    }
    /* enable interrupt */
    rt_hw_interrupt_enable(level);

    RT_DEBUG_LOG(RT_DEBUG_TIMER, ("software timer check leave\n"));
}

/**
 * @brief System timer thread entry
 *
 * @param parameter is the arg of the thread
 */
static void _timer_thread_entry(void *parameter) // 系统 timer 管理线程入口
{
    rt_tick_t next_timeout;

    while (1) // 无限循环
    {
        /* get the next timeout tick */
        if (_timer_list_next_timeout(_soft_timer_list, &next_timeout) != RT_EOK) // 没有找到定时器
        {
            /* no software timer exist, suspend self. */
            rt_thread_suspend(rt_thread_self());
            rt_schedule();
        }
        else
        {
            rt_tick_t current_tick;

            current_tick = rt_tick_get(); /// 获取当前 tick 

            if ((next_timeout - current_tick) < RT_TICK_MAX / 2)
            {
                /* get the delta timeout tick */
                next_timeout = next_timeout - current_tick;
                rt_thread_delay(next_timeout);
            }
        }

        /* check software timer */
        rt_soft_timer_check();
    }
}
#endif /* RT_USING_TIMER_SOFT */

/**
 * @ingroup SystemInit
 *
 * @brief This function will initialize system timer
 */
// 在系统启动时需要初始化定时器管理系统。可以通过下面的函数接口完成
void rt_system_timer_init(void)
{
    int i;

    for (i = 0; i < sizeof(_timer_list) / sizeof(_timer_list[0]); i++)
    {
        rt_list_init(_timer_list + i);
    }
}

/**
 * @ingroup SystemInit
 *
 * @brief This function will initialize system timer thread
 */
// 如果需要使用 SOFT_TIMER，则系统初始化时，应该调用下面这个函数接口
void rt_system_timer_thread_init(void)
{
#ifdef RT_USING_TIMER_SOFT
    int i;

    for (i = 0;
         i < sizeof(_soft_timer_list) / sizeof(_soft_timer_list[0]);
         i++)
    {
        rt_list_init(_soft_timer_list + i);
    }

    /* start software timer thread */
    rt_thread_init(&_timer_thread,
                   "timer",
                   _timer_thread_entry,
                   RT_NULL,
                   &_timer_thread_stack[0],
                   sizeof(_timer_thread_stack),
                   RT_TIMER_THREAD_PRIO,
                   10);

    /* startup */
    rt_thread_startup(&_timer_thread);
#endif /* RT_USING_TIMER_SOFT */
}

/**@}*/
