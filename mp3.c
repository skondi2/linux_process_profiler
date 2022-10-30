#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include "mp3_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_ID");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1

/*

NOTES:
https://linux-kernel-labs.github.io/refs/heads/master/labs/deferred_work.html
https://linux-kernel-labs.github.io/refs/heads/master/labs/memory_mapping.html

*/

struct process_list {
   struct list_head list;
   struct mp3_task_struct* mp3_task;
};
struct process_list* registered_processes;
spinlock_t list_lock;

struct mp3_task_struct {
   struct task_struct linux_task; // PCB
   int pid;
   unsigned long major_fault_count; 
   unsigned long minor_fault_count;
   unsigned long cpu_utilization;
};

struct workqueue_struct* queue;
struct work_struct* work;

int PAGE_SIZE = 4000; // 4 KB
int NUM_PAGES = 128;

// maximum 12,000 samples
// each sample has 4 unsigned longs (4 * 4 bytes = 16 bytes)
// sample = jiffies, minor count, major count, CPU utilization
unsigned long* shared_mem_buffer;
int buffer_pos;
spinlock_t buffer_lock;

void work_callback(struct work_struct* work) {
   unsigned long total_minor_count = 0;
   unsigned long total_major_count = 0;
   unsigned long total_cpu_time = 0;
   
   struct process_list* tmp;
   struct list_head* curr_pos;

   spin_lock_irq(&list_lock);
   list_for_each(curr_pos, &(registered_processes->list)) {
            tmp = list_entry(curr_pos, struct process_list, list);
            int pid = tmp->mp3_task->pid;
            unsigned long minor_count;
            unsigned long major_count;
            unsigned long user_mode_time; // in jiffies
            unsigned long kernel_mode_time; // in jiffies

            int ret = get_cpu_use(pid, &minor_count, &major_count, &user_mode_time, &kernel_mode_time);
            if (ret != 0) {
               printk("MP3 Invalid PID");
            }
            else {
               total_minor_count += minor_count;
               total_major_count += major_count;
               total_cpu_time += ((user_mode_time + kernel_mode_time) / jiffies);
            }
   }
   spin_unlock_irq(&list_lock);

   spin_lock_irq(&buffer_lock);
   if (buffer_pos >= (PAGE_SIZE * NUM_PAGES) / 4) {
      buffer_pos = 0;
   }

   shared_mem_buffer[buffer_pos] = jiffies;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_minor_count;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_major_count;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_cpu_time;
   buffer_pos++;
   spin_unlock_irq(&buffer_lock);

   queue_delayed_work(queue, work, jiffies + msecs_to_jiffies(50));
}

ssize_t proc_read_callback(struct file* file, char __user *buf, size_t size, loff_t* pos) {
   char* data = kmalloc(size, GFP_KERNEL);
   memset(data, 0, size);

   // write to data the linked list node data
   int bytes_read = 0;
   struct process_list* tmp;
   struct list_head* curr_pos;

   spin_lock_irq(&lock);
   list_for_each(curr_pos, &(registered_processes->list)) {
      tmp = list_entry(curr_pos, struct process_list, list);
      int pid = tmp->mp3_task->pid;

      if (pid > *pos) {
         int curr_bytes_read = sprintf(data + bytes_read, "%d\n", pid);
         if (bytes_read + curr_bytes_read >= size) {
            break;
         }
         *pos = pid;
         bytes_read += curr_bytes_read;
      }
   }
   data[bytes_read] = '\0';
   spin_unlock_irq(&lock);

   int success = copy_to_user(buf, data, bytes_read+1);
   if (success != 0) {
      return 0;
   }
   kfree(data);

   return bytes_read; // return the number of bytes that were read
}

ssize_t proc_write_callback(struct file* file, const char __user *buf, size_t size, loff_t* pos) {
   if (*pos != 0) {
      return 0;
   }

   char* buf_cpy = kmalloc(size+1, GFP_KERNEL);
   copy_from_user(buf_cpy, buf, size);
   buf_cpy[size] = '\0';

   char* temp = kmalloc(size+1, GFP_KERNEL);
   strcpy(temp, buf_cpy);
   char* original = temp;

   char operation = buf_cpy[0];
   temp = temp + 2; // remove the first comma

   int pid;
   kstrtoint(temp, 10, &pid);

   if (operation == "R") {
      // add process to linked list
      struct mp3_task_struct* task = kmalloc(sizeof(struct mp3_task_struct), GFP_KERNEL);
      memset(task, 0, sizeof(mp3_task_struct));
      task->linux_task = find_task_by_pid(pid); // TODO: initialize major, minor, utilization?
      task->pid = pid;

      struct process_list* node = kmalloc(sizeof(struct process_list), GFP_KERNEL);
      memset(node, 0, sizeof(process_list));
      node->mp3_task = task;
      INIT_LIST_HEAD(&(node->list));

      spin_lock_irq(&list_lock);
      list_add_tail(&(tmp->list), &(registered_processes->list));

      // create work/job if first pid registered
      if (list_empty(&(registered_processes->list))) { 
         work = kmalloc(sizeof(struct work_struct), GFP_KERNEL);
         INIT_WORK(work, work_callback);
         queue_delayed_work(queue, work, jiffies + msecs_to_jiffies(50));
      }
      spin_unlock_irq(&list_lock);
   }
   else if (operation == "U") {
      // delete process from linked list
      struct process_list* tmp;
      struct list_head *curr_pos, *q;

      spin_lock_irq(&list_lock);
      list_for_each_safe(curr_pos, q, &(registered_processes->list)) {
         tmp = list_entry(curr_pos, struct process_list, list);
         
         if (tmp->mp3_task->pid == pid) {
            list_del(curr_pos);
            kfree(tmp);

            // if list is empty, job is also deleted
            if (list_empty(&(registered_processes->list))) { 
               flush_work(work);
               cancel_work_sync(work);
               kfree(work);
            }
         }
      }
      spin_unlock_irq(&list_lock);
   }

   kfree(original);
   return size;
}

const struct proc_ops proc_fops = {
   .proc_read = proc_read_callback,
   .proc_write = proc_write_callback,
};

struct proc_dir_entry* proc_dir;
struct proc_dir_entry* proc_file;

static int my_open(struct inode *inode, struct file *file) {
   return 0;
}

static int my_release(struct inode *inode, struct file *file) {
   return 0;
}

const struct file_operations dev_fops = {
   .open = my_open;
   .mmap = my_mmap;
   .release = my_release;
   .owner = THIS_MODULE;
};

// mp1_init - Called when module is loaded
int __init mp3_init(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE LOADING\n");
   #endif
   
   proc_dir = proc_mkdir("mp3", NULL);
   proc_file = proc_create("status", 0666, proc_dir, &proc_fops);
   
   registered_processes - kmalloc(sizeof(struct process_list), GFP_KERNEL);
   INIT_LIST_HEAD(&(registered_processes->list));

   spin_lock_init(&list_lock);
   spin_lock_init(&buffer_lock);

   queue = create_workqueue("queue");

   // allocate memory buffer
   shared_mem_buffer = vmalloc(PAGE_SIZE * NUM_PAGES);
   buffer_pos = 0;
   for (int i = 0; i < NUM_PAGES * PAGE_SIZE; i += PAGE_SIZE) {
      // prevent the pages from being swapped out to disk
      SetPageReserved(virt_to_page(((unsigned long)shared_mem_buffer) + i));
   }
   
   printk(KERN_ALERT "MP3 MODULE LOADED\n");
   return 0;   
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif

   destroy_workqueue(queue);

   struct process_list* tmp;
   struct list_head *pos, *q;
   list_for_each_safe(pos, q, &(registered_processes->list)) {
      tmp = list_entry(pos, struct process_list, list);
      list_del(pos);
      kfree(tmp);
   }

   // deallocate memory buffer
   for (int i = 0; i < NUM_PAGES * PAGE_SIZE; i += PAGE_SIZE) {
      ClearPageReserved(virt_to_page(((unsigned long)shared_mem_buffer) + i));
   }
   vfree(shared_mem_buffer);
   
   remove_proc_entry("status", proc_dir);
   remove_proc_entry("mp3", NULL);

   printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
