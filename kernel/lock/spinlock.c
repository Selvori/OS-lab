#include <kernel/lock.h>
#include <kernel/arch.h>
#include <kernel/print.h>

// TODO(lab-1): 初始化未持有的锁；owner 为 -1。
void spinlock_init(spinlock_t *lk, const char *name)
{ 
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lk->owner, -1, __ATOMIC_RELAXED);
    lk->name = name;
}

// TODO(lab-1): 判断当前 CPU 是否持有锁。
bool spinlock_holding(spinlock_t *lk)
{
    return __atomic_load_n(&lk->locked, __ATOMIC_RELAXED) != 0 && __atomic_load_n(&lk->owner, __ATOMIC_RELAXED) == (int)arch_cpu_id();
}

// TODO(lab-1): push_off 后原子获取锁，记录持有者。
void spinlock_acquire(spinlock_t *lk)
{
    /*
     * 必须在争锁前关闭本 CPU 的中断，防止中断处理程序
     * 再次申请当前执行流正在持有或等待的锁。
     */
    push_off();


    /*
     * 自旋锁不可重入。同一 CPU 再次申请自己持有的锁
     * 将永远无法成功，因此直接报告错误。
     */
    if (spinlock_holding(lk))
        panic("spinlock_acquire: already holding");

    /*
     * 原子地将 locked 设置为 1。
     * 只有读到旧值 0 的 CPU 能成功离开循环。
     */
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0) {
    }

    __atomic_store_n(&lk->owner, (int)arch_cpu_id(), __ATOMIC_RELAXED);
}

// TODO(lab-1): 检查持有者，发布受保护写入，解锁并 pop_off。
void spinlock_release(spinlock_t *lk)
{
    /*
     * 只有当前持锁 CPU 才能释放这把锁。
     */
    if (!spinlock_holding(lk))
        panic("spinlock_release: not owner");

    __atomic_store_n(&lk->owner, -1, __ATOMIC_RELAXED);

    /*
     * RELEASE 保证临界区内的内存写入在其他 CPU
     * 看到 locked == 0 之前完成。
     */
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);

    /*
     * 必须在真正解锁之后恢复中断。
     */
    pop_off();
}
