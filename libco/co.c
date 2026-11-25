#include "co.h"
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdnoreturn.h>

#define STACK_SIZE 64 * 1024 // 默认协程栈大小为 64KB
#define MAX_CO 128           // 最大协程数量

#if __x86_64__
// x86_64 ABI 要求 call 之前 rsp 必须 16 字节对齐
// 因此进入函数体之前。rsp % 16 == 8
// jmp 进入函数体没有压入返回地址
// 所以需要手动调整 rsp 满足函数入口状态
#define OFFSET 8
#else
#define OFFSET 12
#endif

// 协程状态枚举
enum co_status {
  CO_NEW = 1, // 新创建，尚未执行
  CO_RUNNING, // 正在执行的协程
  CO_WAITING, // 在 co_wait 上等待
  CO_DEAD,    // 执行完毕，等待释放资源
};

// 协程数据结构定义
struct co {
  char *name;           // 协程名称
  void (*func)(void *); // co_start 指定的函数入口
  void *arg;            // co_start 指定的函数参数

  enum co_status status; // 协程状态
  struct co *waiter; // 正在等待该协程的协程，若没有则为 NULL
  jmp_buf context; // 被保存的寄存器状态，保存协程切换时的上下文
  uint8_t stack[STACK_SIZE]; // 协程栈空间
};

// 当前正在执行的协程
static struct co *current = NULL;
static int co_count = 0; // 当前分配的协程数量
struct co *all_cos[MAX_CO]; // 保存所有协程的指针，最多支持 128 个协程

static void all_add(struct co *co) {
  assert(co_count < MAX_CO);
  all_cos[co_count++] = co;
}

static void all_remove(struct co *co) {
  for (int i = 0; i < co_count; i++) {
    if (all_cos[i] == co) {
      // 交换到最后一个位置，减少数组长度
      all_cos[i] = all_cos[--co_count];
      return;
    }
  }
}

// 创建协程，类似于 pthread_create
// 新协程不会立刻执行，而是等待调用 co_wait 或 co_yield 才会切换到新协程
struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  struct co *new_co = (struct co *)malloc(sizeof(struct co));
  if (new_co == NULL) {
    return NULL;
  }
  new_co->name = (char *)name;
  new_co->func = func;
  new_co->arg = arg;
  new_co->status = CO_NEW;
  new_co->waiter = NULL;

  all_add(new_co);
  return new_co;
}

// 等待协程执行完毕，类似于 pthread_join
// 目标协程结束后，co_wait 返回之前，释放协程的资源
// 每个协程应该只被 co_wait 一次，否则会出现内存泄漏
// main 也是协程，可以调用 co_yield 和 co_wait
void co_wait(struct co *co) {
  // 被等待的线程有两种情况：已经结束，或者还在运行
  if (co->status != CO_DEAD) {
    // 等待的协程还没结束
    // 将当前协程设置为等待状态
    current->status = CO_WAITING;
    co->waiter = current; // 设置等待协程
    // 等到协程执行完毕后，唤醒当前协程
    // 在此之前，交出 CPU，切换到其他协程执行
    while (co->status != CO_DEAD) {
      co_yield ();
    }
    // 等待的协程已经结束，当前协程被唤醒，继续执行
    current->status = CO_RUNNING;
  }
  all_remove(co);
  free(co);
  return;
}

// 从可选择的协程中选择一个协程执行
// 为了保证选取随机，使用水塘抽样算法
static struct co *pick_next_co() {
  struct co *pick = NULL;
  int m = 0; // 记录当前可运行协程的数量
  for (int i = 0; i < co_count; i++) {
    struct co *c = all_cos[i];
    if (c->status == CO_NEW || c->status == CO_RUNNING) {
      if (rand() % ++m == 0)
        pick = c; // 以 1/m 的概率选择当前协程
    }
  }
  return pick;
}

/**
 * @brief 切换到指定的栈执行函数，并传递参数
 *
 * @param sp 目标栈指针
 * @param entry 目标函数入口
 * @param arg 目标函数参数
 */
static inline void stack_switch_call(void *sp, void *entry, uintptr_t arg) {
  asm volatile(
#if __x86_64__
      // 64 位系统，使用寄存器 %rdi, %rsi, %rdx 分别传前三个参数，栈指针使用
      // %rsp sp 放在 %rbx，entry 放在 %rdx，arg 放在 %rax
      "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
      :
      : "b"((uintptr_t)sp), "d"(entry), "a"(arg)
      : "memory"
#else
      // 32 位系统，栈传参，esp = sp - 8 保证 16 字节对齐
      // esp + 0 是伪返回地址，esp + 4 是函数参数 arg
      "movl %0, %%esp; movl %2, 4(%0); jmp *%1"
      :
      : "b"((uintptr_t)sp - 8), "d"(entry), "a"(arg)
      : "memory"
#endif
  );
}

/**
 * @brief 协程切换的入口函数
 * @note 函数是在 stack_switch_call 中被调用的，没有保存返回地址
 *        因此该函数不能返回，否则会跳转到未知地址，导致程序崩溃
 * @param co 目标协程
 */
[[noreturn]] static void co_entry(struct co *co) {
  co->func(co->arg);    // 执行协程函数
  co->status = CO_DEAD; // 执行完毕
  // 如果有协程在等待该协程执行完毕，唤醒它
  if (co->waiter) {
    // 切换到等待协程的上下文
    co->waiter->status = CO_RUNNING;
    current = co->waiter;
    longjmp(co->waiter->context, 1);
  }
  // 让出 CPU，切换到其他协程执行
  // 永不返回
  for (;;) {
    co_yield ();
  }
}

// 当前协程主动让出 CPU，切换到其他协程执行
// 保存寄存器状态，切换到其他协程执行
// 在所有可运行协程中选择一个协程执行
// 直到该协程调用 co_yield 或返回
// main 返回后，进程立刻退出，不会等待其他协程执行完毕
void co_yield () {
  // 跳转到其他协程执行
  volatile int val = setjmp(current->context);
  if (val == 0) {
    // setjmp 第一次返回，现场已经被保存
    // 选择一个可运行的协程（NEW 或 RUNNING）
    struct co *next = pick_next_co();
    if (next == NULL) {
      // 没有可运行的协程，直接返回
      return;
    }
    // 否则将当前协程切换到 next 协程执行
    current = next;
    // 如果 next 是新创建的协程，调用其入口函数执行
    if (next->status == CO_NEW) {
      // 调用 stack_switch_call 切换到 next 协程的栈执行
      next->status = CO_RUNNING;
      stack_switch_call(next->stack + STACK_SIZE - OFFSET, (void *)co_entry,
                        (uintptr_t)next);
    } else {
      // 否则直接切换到 next 协程执行
      longjmp(next->context, 1);
    }
  } else {
    // setjmp 第二次返回，说明当前协程被唤醒，继续执行
    return;
  }
}

// 提供 main 函数的协程入口，main 也是协程
__attribute__((constructor)) static void co_init() {
  // 初始化 main 协程
  static struct co main_co;
  main_co.name = "main";
  main_co.func = NULL;
  main_co.arg = NULL;
  main_co.status = CO_RUNNING;
  main_co.waiter = NULL;
  current = &main_co;
  all_add(&main_co);
}