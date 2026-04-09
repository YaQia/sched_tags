void my_cross_process_lock(void);
void my_cross_process_unlock(void);

void worker() {
    my_cross_process_lock();
    // 临界区
    my_cross_process_unlock();
}
