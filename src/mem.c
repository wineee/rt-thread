/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2008-7-12      Bernard      the first version
 * 2010-06-09     Bernard      fix the end stub of heap
 *                             fix memory check in rt_realloc function
 * 2010-07-13     Bernard      fix RT_ALIGN issue found by kuronca
 * 2010-10-14     Bernard      fix rt_realloc issue when realloc a NULL pointer.
 * 2017-07-14     armink       fix rt_realloc issue when new size is 0
 * 2018-10-02     Bernard      Add 64bit support
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

/*
在 include/rtdef.h 中，可以找到内核对象 rt_memory 的定义：
struct rt_memory
{
    struct rt_object        parent;           // 继承自 rt_object
    const char *            algorithm;        // 内存管理算法的名称
    rt_ubase_t              address;          // 内存空间首地址
    rt_size_t               total;            // 空间大小
    rt_size_t               used;             // 已经使用空间的大小
    rt_size_t               max;              // 内存最大使用量
};
typedef struct rt_memory *rt_mem_t;
typedef rt_mem_t rt_smem_t;

*/

#include <rthw.h>
#include <rtthread.h>

#if defined (RT_USING_SMALL_MEM)
 /**
  * memory item on the small mem
  */
struct rt_small_mem_item
{
    rt_ubase_t              pool_ptr;         // 小内存块对象地址/
#ifdef ARCH_CPU_64BIT
    rt_uint32_t             resv;
#endif /* ARCH_CPU_64BIT */
    rt_size_t               next;             // 下一个未使用的内存块
    rt_size_t               prev;              // 上一个未使用的内存块
#ifdef RT_USING_MEMTRACE
#ifdef ARCH_CPU_64BIT
    rt_uint8_t              thread[8];       /**< thread name */
#else
    rt_uint8_t              thread[4];       /**< thread name */
#endif /* ARCH_CPU_64BIT */
#endif /* RT_USING_MEMTRACE */
};

/**
 * Base structure of small memory object
 */
struct rt_small_mem
{
    struct rt_memory            parent;                 // 继承自 rt_memory
    rt_uint8_t                 *heap_ptr;               // 堆地址指针
    struct rt_small_mem_item   *heap_end;              // 堆尾内存块，标志结束
    struct rt_small_mem_item   *lfree;      // 始终指向剩余可用空间的第一个空闲内存块
    rt_size_t                   mem_size_aligned;       // 对齐后的内存大小
};

#define HEAP_MAGIC 0x1ea0

#ifdef ARCH_CPU_64BIT
#define MIN_SIZE 24
#else
#define MIN_SIZE 12
#endif /* ARCH_CPU_64BIT */

#define MEM_MASK             0xfffffffe
#define MEM_USED()         ((((rt_base_t)(small_mem)) & MEM_MASK) | 0x1)
#define MEM_FREED()        ((((rt_base_t)(small_mem)) & MEM_MASK) | 0x0)
#define MEM_ISUSED(_mem)   \
                      (((rt_base_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (~MEM_MASK))
#define MEM_POOL(_mem)     \
    ((struct rt_small_mem *)(((rt_base_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (MEM_MASK)))
#define MEM_SIZE(_heap, _mem)      \
    (((struct rt_small_mem_item *)(_mem))->next - ((rt_ubase_t)(_mem) - \
    (rt_ubase_t)((_heap)->heap_ptr)) - RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE))

#define MIN_SIZE_ALIGNED     RT_ALIGN(MIN_SIZE, RT_ALIGN_SIZE)
#define SIZEOF_STRUCT_MEM    RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE)

#ifdef RT_USING_MEMTRACE
rt_inline void rt_smem_setname(struct rt_small_mem_item *mem, const char *name)
{
    int index;
    for (index = 0; index < sizeof(mem->thread); index ++)
    {
        if (name[index] == '\0') break;
        mem->thread[index] = name[index];
    }

    for (; index < sizeof(mem->thread); index ++)
    {
        mem->thread[index] = ' ';
    }
}
#endif /* RT_USING_MEMTRACE */

static void plug_holes(struct rt_small_mem *m, struct rt_small_mem_item *mem)
{
    struct rt_small_mem_item *nmem;
    struct rt_small_mem_item *pmem;

    RT_ASSERT((rt_uint8_t *)mem >= m->heap_ptr);
    RT_ASSERT((rt_uint8_t *)mem < (rt_uint8_t *)m->heap_end);

    /* plug hole forward */
    nmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next];
    if (mem != nmem && !MEM_ISUSED(nmem) &&
        (rt_uint8_t *)nmem != (rt_uint8_t *)m->heap_end)
    {
        /* if mem->next is unused and not end of m->heap_ptr,
         * combine mem and mem->next
         */
        if (m->lfree == nmem)
        {
            m->lfree = mem;
        }
        nmem->pool_ptr = 0;
        mem->next = nmem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[nmem->next])->prev = (rt_uint8_t *)mem - m->heap_ptr;
    }

    /* plug hole backward */
    pmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->prev];
    if (pmem != mem && !MEM_ISUSED(pmem))
    {
        /* if mem->prev is unused, combine mem and mem->prev */
        if (m->lfree == mem)
        {
            m->lfree = pmem;
        }
        mem->pool_ptr = 0;
        pmem->next = mem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[mem->next])->prev = (rt_uint8_t *)pmem - m->heap_ptr;
    }
}

/**
 * @brief This function will initialize small memory management algorithm.
 *
 * @param m the small memory management object.
 *
 * @param name is the name of the small memory management object.
 *
 * @param begin_addr the beginning address of memory.
 *
 * @param size is the size of the memory.
 *
 * @return Return a pointer to the memory object. When the return value is RT_NULL, it means the init failed.
 */
// 由源代码可知，初始化时小内存管理算法通过传进来的起始地址和末尾地址将动态堆内存初始化为两个内存块：
// 第一个内存块指向动态堆内存首地址，可用空间为整个可分配的内存，此内存块next指针指向末尾内存控制块；
// 第二个内存块指向最末尾的一个内存控制块，可用空间大小为0

rt_smem_t rt_smem_init(const char    *name, // 内存管理对象的名字
                     void          *begin_addr, // 内存起始地址
                     rt_size_t      size) // 内存大小
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;
    rt_ubase_t start_addr, begin_align, end_align, mem_size;
    /* 在内存起始的位置分配一段空间给 small_mem */
    small_mem = (struct rt_small_mem *)RT_ALIGN((rt_ubase_t)begin_addr, RT_ALIGN_SIZE);
    start_addr = (rt_ubase_t)small_mem + sizeof(*small_mem);
    /* 确保内存对齐 */
    begin_align = RT_ALIGN((rt_ubase_t)start_addr, RT_ALIGN_SIZE);
    end_align   = RT_ALIGN_DOWN((rt_ubase_t)begin_addr + size, RT_ALIGN_SIZE);
    /* 确保分配 2 个 mem 还有空间可以用 */
    if ((end_align > (2 * SIZEOF_STRUCT_MEM)) &&
        ((end_align - 2 * SIZEOF_STRUCT_MEM) >= start_addr))
    {
        /* 注意 mem_size 减去了 2 * SIZEOF_STRUCT_MEM */
        mem_size = end_align - begin_align - 2 * SIZEOF_STRUCT_MEM; 
    }
    else
        return RT_NULL;
    /* 将位于内存块首地址的 small_mem 数据清 0 */
    rt_memset(small_mem, 0, sizeof(*small_mem));
    /* 为位于内存块首地址的内存管理对象 small_mem 赋值 */
    rt_object_init(&(small_mem->parent.parent), RT_Object_Class_Memory, name);
    small_mem->parent.algorithm = "small";
    small_mem->parent.address = begin_align;
    small_mem->parent.total = mem_size;
    small_mem->mem_size_aligned = mem_size;
    small_mem->heap_ptr = (rt_uint8_t *)begin_align; // 堆空间的起始地址
    /* 初始化 heap start */
    mem        = (struct rt_small_mem_item *)small_mem->heap_ptr;
    mem->pool_ptr = MEM_FREED(); // 标记为未使用
    mem->next  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM; // 下一个内存块指向 heap_end
    mem->prev  = 0; // 没有前一个
    /* 初始化 heap_end */
    small_mem->heap_end        = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next];
    small_mem->heap_end->pool_ptr = MEM_USED(); // 可用空间大小为0
    /* 此内存块前一指针和后一指针都指向本身 */
    small_mem->heap_end->next  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    small_mem->heap_end->prev  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    /* 空闲指针lfree，初始化时指向动态内存堆起始地址 */
    small_mem->lfree = (struct rt_small_mem_item *)small_mem->heap_ptr;
    return &small_mem->parent;
}
RTM_EXPORT(rt_smem_init);

/**
 * @brief This function will remove a small mem from the system.
 *
 * @param m the small memory management object.
 *
 * @return RT_EOK
 */
rt_err_t rt_smem_detach(rt_smem_t m)
{
    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    rt_object_detach(&(m->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_smem_detach);

/**
 * @addtogroup MM
 */

/**@{*/

/**
 * @brief Allocate a block of memory with a minimum of 'size' bytes.
 *
 * @param m the small memory management object.
 *
 * @param size is the minimum size of the requested block in bytes.
 *
 * @return the pointer to allocated memory or NULL if no free memory was found.
 */
//rt_smem_alloc，申请一段大小为 size 的内存
void *rt_smem_alloc(rt_smem_t m, 
                    rt_size_t size)
{
    rt_size_t ptr, ptr2;
    struct rt_small_mem_item *mem, *mem2;
    struct rt_small_mem *small_mem;
    if (size == 0)
        return RT_NULL;
    /* 参数合法性判断 */
    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));
    /* 根据内存对齐规则，如果 size 不是 RT_ALIGN_SIZE 整倍数，输出警告 */
    if (size != RT_ALIGN(size, RT_ALIGN_SIZE))
        RT_DEBUG_LOG(RT_DEBUG_MEM, ("malloc size %d, but align to %d\n",
                                    size, RT_ALIGN(size, RT_ALIGN_SIZE)));
    else
        RT_DEBUG_LOG(RT_DEBUG_MEM, ("malloc size %d\n", size));

    small_mem = (struct rt_small_mem *)m; // 转换成子类 rt_small_mem
    size = RT_ALIGN(size, RT_ALIGN_SIZE); // 确保内存对齐
    /* 分配的内存块大小不低于 MIN_SIZE_ALIGNED */
    if (size < MIN_SIZE_ALIGNED)
        size = MIN_SIZE_ALIGNED;
    /* 如果剩余空间不足够分配，直接失败，返回 RT_NULL */
    if (size > small_mem->mem_size_aligned)
    {
        RT_DEBUG_LOG(RT_DEBUG_MEM, ("no memory\n"));
        return RT_NULL;
    }    
    /* 遍历空闲内存链表 */
    for (ptr = (rt_uint8_t *)small_mem->lfree - small_mem->heap_ptr; // ptr 是相对首地址的偏移量
         ptr <= small_mem->mem_size_aligned - size;
         ptr = ((struct rt_small_mem_item *)&small_mem->heap_ptr[ptr])->next)
    {
        mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr];

        if ((!MEM_ISUSED(mem)) && (mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size) 
        {
            /* mem 没有被使用 并且可用空间足够分配 size 大小的内存加上 SIZEOF_STRUCT_MEM 大小的内存控制块 */
            if (mem->next - (ptr + SIZEOF_STRUCT_MEM) >=
                (size + SIZEOF_STRUCT_MEM + MIN_SIZE_ALIGNED))
            {
                /* 如果分配后多余的空间大于 MIN_SIZE_ALIGNED，那就有必要分裂一下内存块 */
                ptr2 = ptr + SIZEOF_STRUCT_MEM + size; 
                /* 初始化 mem2 */
                mem2       = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
                mem2->pool_ptr = MEM_FREED();
                /* 把新分出的内存块加入链表中， mem 的后面 */
                mem2->next = mem->next;
                mem2->prev = ptr;
                mem->next = ptr2;
                if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
                {
                    // 如果 mem2 的下一个内存块如果不是 heap_end，更新它的链表信息
                    // heap_end 的 prev 是 heap_end 自己，不需要更新
                    ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
                }
                small_mem->parent.used += (size + SIZEOF_STRUCT_MEM); // 更新已分配内存总量
                if (small_mem->parent.max < small_mem->parent.used) // 更新内存最大使用量
                    small_mem->parent.max = small_mem->parent.used;
            }
            else
            {
                /* 分配后剩下的空间不足以新建一个内存块，直接使用该内存块就可以 */
                small_mem->parent.used += mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr);
                if (small_mem->parent.max < small_mem->parent.used)
                    small_mem->parent.max = small_mem->parent.used;
            }
            /* 标记已使用 */
            mem->pool_ptr = MEM_USED();
            if (mem == small_mem->lfree)
            {
                /* 如果巧好把 lfree 对应内存块分配了，找到第一个未分配的内存块，更新 lfree */
                while (MEM_ISUSED(small_mem->lfree) && small_mem->lfree != small_mem->heap_end)
                    small_mem->lfree = (struct rt_small_mem_item *)&small_mem->heap_ptr[small_mem->lfree->next];

                RT_ASSERT(((small_mem->lfree == small_mem->heap_end) || (!MEM_ISUSED(small_mem->lfree))));
            }

            /* 返回分配的内存地址，注意内存控制块对用户不可见，所以要加上 SIZEOF_STRUCT_MEM */
            return (rt_uint8_t *)mem + SIZEOF_STRUCT_MEM;
        }
    }

    return RT_NULL;
}

RTM_EXPORT(rt_smem_alloc);

/**
 * @brief This function will change the size of previously allocated memory block.
 *
 * @param m the small memory management object.
 *
 * @param rmem is the pointer to memory allocated by rt_mem_alloc.
 *
 * @param newsize is the required new size.
 *
 * @return the changed memory block address.
 */
/*
此外，还有一个 rt_smem_reallo 函数，可以重新分配内存块大小，但是只能变小，不可以增大。也就是分裂内存块，前一块是新的空间大小，继续使用，后一块是剩余的空间，标记为空闲，如果后面的内存块也是空闲，则合并。
*/
void *rt_smem_realloc(rt_smem_t m, void *rmem, rt_size_t newsize)
{
    rt_size_t size;
    rt_size_t ptr, ptr2;
    struct rt_small_mem_item *mem, *mem2;
    struct rt_small_mem *small_mem;
    void *nmem;

    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    small_mem = (struct rt_small_mem *)m;
    /* alignment size */
    newsize = RT_ALIGN(newsize, RT_ALIGN_SIZE);
    if (newsize > small_mem->mem_size_aligned)
    {
        RT_DEBUG_LOG(RT_DEBUG_MEM, ("realloc: out of memory\n"));

        return RT_NULL;
    }
    else if (newsize == 0)
    {
        rt_smem_free(rmem);
        return RT_NULL;
    }

    /* allocate a new memory block */
    if (rmem == RT_NULL)
        return rt_smem_alloc(&small_mem->parent, newsize);

    RT_ASSERT((((rt_ubase_t)rmem) & (RT_ALIGN_SIZE - 1)) == 0);
    RT_ASSERT((rt_uint8_t *)rmem >= (rt_uint8_t *)small_mem->heap_ptr);
    RT_ASSERT((rt_uint8_t *)rmem < (rt_uint8_t *)small_mem->heap_end);

    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);

    /* current memory block size */
    ptr = (rt_uint8_t *)mem - small_mem->heap_ptr;
    size = mem->next - ptr - SIZEOF_STRUCT_MEM;
    if (size == newsize)
    {
        /* the size is the same as */
        return rmem;
    }

    if (newsize + SIZEOF_STRUCT_MEM + MIN_SIZE < size)
    {
        /* split memory block */
        small_mem->parent.used -= (size - newsize);

        ptr2 = ptr + SIZEOF_STRUCT_MEM + newsize;
        mem2 = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
        mem2->pool_ptr = MEM_FREED();
        mem2->next = mem->next;
        mem2->prev = ptr;
#ifdef RT_USING_MEMTRACE
        rt_smem_setname(mem2, "    ");
#endif /* RT_USING_MEMTRACE */
        mem->next = ptr2;
        if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
        {
            ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
        }

        if (mem2 < small_mem->lfree)
        {
            /* the splited struct is now the lowest */
            small_mem->lfree = mem2;
        }

        plug_holes(small_mem, mem2);

        return rmem;
    }

    /* expand memory */
    nmem = rt_smem_alloc(&small_mem->parent, newsize);
    if (nmem != RT_NULL) /* check memory */
    {
        rt_memcpy(nmem, rmem, size < newsize ? size : newsize);
        rt_smem_free(rmem);
    }

    return nmem;
}
RTM_EXPORT(rt_smem_realloc);

/**
 * @brief This function will release the previously allocated memory block by
 *        rt_mem_alloc. The released memory block is taken back to system heap.
 *
 * @param rmem the address of memory which will be released.
 */
void rt_smem_free(void *rmem)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;
    if (rmem == RT_NULL)
        return;
    /* 根据用户可见内存地址计算出内存控制块地址 */
    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);
    small_mem = MEM_POOL(mem);
    mem->pool_ptr = MEM_FREED(); // 标记为空闲
    /* 更新 lfree，使其始终指向剩余可用空间的第一个空闲内存块 */
    if (mem < small_mem->lfree)
        small_mem->lfree = mem;
    /* 更新已用内存 */
    small_mem->parent.used -= (mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr));
    /* 如果前一个/后一个内存块也是空闲的，则合并 */
    plug_holes(small_mem, mem);
}

RTM_EXPORT(rt_smem_free);

#ifdef RT_USING_FINSH
#include <finsh.h>

#ifdef RT_USING_MEMTRACE
int memcheck(int argc, char *argv[])
{
    int position;
    rt_ubase_t level;
    struct rt_small_mem_item *mem;
    struct rt_small_mem *m;
    struct rt_object_information *information;
    struct rt_list_node *node;
    struct rt_object *object;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    level = rt_hw_interrupt_disable();
    /* get mem object */
    information = rt_object_get_information(RT_Object_Class_Memory);
    for (node = information->object_list.next;
         node != &(information->object_list);
         node  = node->next)
    {
        object = rt_list_entry(node, struct rt_object, list);
        /* find the specified object */
        if (name != RT_NULL && rt_strncmp(name, object->name, RT_NAME_MAX) != 0)
            continue;
        /* mem object */
        m = (struct rt_small_mem *)object;
        /* check mem */
        for (mem = (struct rt_small_mem_item *)m->heap_ptr; mem != m->heap_end; mem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next])
        {
            position = (rt_ubase_t)mem - (rt_ubase_t)m->heap_ptr;
            if (position < 0) goto __exit;
            if (position > (int)m->mem_size_aligned) goto __exit;
            if (MEM_POOL(mem) != m) goto __exit;
        }
    }
    rt_hw_interrupt_enable(level);

    return 0;
__exit:
    rt_kprintf("Memory block wrong:\n");
    rt_kprintf("   name: %s\n", m->parent.parent.name);
    rt_kprintf("address: 0x%08x\n", mem);
    rt_kprintf("   pool: 0x%04x\n", mem->pool_ptr);
    rt_kprintf("   size: %d\n", mem->next - position - SIZEOF_STRUCT_MEM);
    rt_hw_interrupt_enable(level);

    return 0;
}
MSH_CMD_EXPORT(memcheck, check memory data);

int memtrace(int argc, char **argv)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *m;
    struct rt_object_information *information;
    struct rt_list_node *node;
    struct rt_object *object;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    /* get mem object */
    information = rt_object_get_information(RT_Object_Class_Memory);
    for (node = information->object_list.next;
         node != &(information->object_list);
         node  = node->next)
    {
        object = rt_list_entry(node, struct rt_object, list);
        /* find the specified object */
        if (name != RT_NULL && rt_strncmp(name, object->name, RT_NAME_MAX) != 0)
            continue;
        /* mem object */
        m = (struct rt_small_mem *)object;
        /* show memory information */
        rt_kprintf("\nmemory heap address:\n");
        rt_kprintf("name    : %s\n", m->parent.parent.name);
        rt_kprintf("total   : 0x%d\n", m->parent.total);
        rt_kprintf("used    : 0x%d\n", m->parent.used);
        rt_kprintf("max_used: 0x%d\n", m->parent.max);
        rt_kprintf("heap_ptr: 0x%08x\n", m->heap_ptr);
        rt_kprintf("lfree   : 0x%08x\n", m->lfree);
        rt_kprintf("heap_end: 0x%08x\n", m->heap_end);
        rt_kprintf("\n--memory item information --\n");
        for (mem = (struct rt_small_mem_item *)m->heap_ptr; mem != m->heap_end; mem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next])
        {
            int size = MEM_SIZE(m, mem);

            rt_kprintf("[0x%08x - ", mem);
            if (size < 1024)
                rt_kprintf("%5d", size);
            else if (size < 1024 * 1024)
                rt_kprintf("%4dK", size / 1024);
            else
                rt_kprintf("%4dM", size / (1024 * 1024));

            rt_kprintf("] %c%c%c%c", mem->thread[0], mem->thread[1], mem->thread[2], mem->thread[3]);
            if (MEM_POOL(mem) != m)
                rt_kprintf(": ***\n");
            else
                rt_kprintf("\n");
        }
    }
    return 0;
}
MSH_CMD_EXPORT(memtrace, dump memory trace information);
#endif /* RT_USING_MEMTRACE */
#endif /* RT_USING_FINSH */

#endif /* defined (RT_USING_SMALL_MEM) */

/**@}*/
