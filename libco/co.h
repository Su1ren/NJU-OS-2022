#pragma once

// co.h - coroutine library

/**
 * @brief 启动一个新的协程
 *
 * @param name 新协程的名字
 * @param func 新协程的执行函数
 * @param arg 新协程的参数
 * @return struct co* 返回新协程的指针
 */
struct co *co_start(const char *name, void (*func)(void *), void *arg);

/**
 * @brief 让当前协程让出控制权
 * @note 当前协程会被挂起，直到其他协程调用 co_wait() 或 co_yield() 才会恢复执行
 */
void co_yield ();

/**
 * @brief 等待指定协程执行完毕
 *
 * @param co 需要等待的协程指针
 */
void co_wait(struct co *co);
