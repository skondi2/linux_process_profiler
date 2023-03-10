#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
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

struct mp3_task_struct {
   struct task_struct* linux_task; // PCB
   int pid;
};

struct process_list {
   struct list_head list;
   struct mp3_task_struct* mp3_task;
};
struct process_list* registered_processes;
spinlock_t list_lock;

struct workqueue_struct* queue;
struct delayed_work* work;

int BUF_PAGE_SIZE = 4096; // 4 KB
int BUF_NUM_PAGES = 128;

// buffer has 524,288 bytes. 
// maximum sample size is 12000 * 16 = 192,000 bytes.
// buffer_pos will go from [0, 48000) unsigned longs

// maximum 12,000 samples
// each sample has 4 unsigned longs (4 * 4 bytes = 16 bytes)
// sample = jiffies, minor count, major count, CPU utilization
unsigned long* shared_mem_buffer;
int buffer_pos;
spinlock_t buffer_lock;

struct cdev* cdev;

void work_callback(struct work_struct* work_) {
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
            unsigned long user_mode_time; // in nanoseconds
            unsigned long kernel_mode_time; // in nanoseconds

            int ret = get_cpu_use(pid, &minor_count, &major_count, &user_mode_time, &kernel_mode_time);
            if (ret != 0) {
               printk("MP3 Invalid PID");
            }
            else {
               total_minor_count += minor_count;
               total_major_count += major_count;
               total_cpu_time += (user_mode_time + kernel_mode_time); 
            }
   }
   spin_unlock_irq(&list_lock);

   //printk("MP3 %ld %ld %ld %ld\n", jiffies, total_minor_count, total_major_count, total_cpu_time);
   spin_lock_irq(&buffer_lock);
   shared_mem_buffer[buffer_pos] = jiffies;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_minor_count;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_major_count;
   buffer_pos++;
   shared_mem_buffer[buffer_pos] = total_cpu_time;
   buffer_pos++;

   if (buffer_pos + 1 == 48000) {
      //printk("MP3 Resetting buffer pos");
      buffer_pos = 0;
   }
   spin_unlock_irq(&buffer_lock);
   
   struct delayed_work* delayed_work = container_of(work_, struct delayed_work, work);
   if (delayed_work == NULL) {
      printk("MP3 container_of failed\n");
   }
   queue_delayed_work(queue, delayed_work, msecs_to_jiffies(50));
}

ssize_t proc_read_callback(struct file* file, char __user *buf, size_t size, loff_t* pos) {
   char* data = kmalloc(size, GFP_KERNEL);
   memset(data, 0, size);

   // write to data the linked list node data
   int bytes_read = 0;
   struct process_list* tmp;
   struct list_head* curr_pos;

   spin_lock_irq(&list_lock);
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
   spin_unlock_irq(&list_lock);

   int success = copy_to_user(buf, data, bytes_read+1);
   if (success != 0) {
      printk("MP3 copy_to_user failed");
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
   int success = copy_from_user(buf_cpy, buf, size);
   if (success != 0) {
      printk("MP3 copy_from_user failed");
      return 0;
   }
   buf_cpy[size] = '\0';

   char* temp = kmalloc(size+1, GFP_KERNEL);
   strcpy(temp, buf_cpy);
   char* original = temp;

   char operation = buf_cpy[0];
   temp = temp + 2; // remove the first comma

   int pid;
   success = kstrtoint(temp, 10, &pid);

   if (operation == 'R') {
      // add process to linked list
      struct mp3_task_struct* task = kmalloc(sizeof(struct mp3_task_struct), GFP_KERNEL);
      memset(task, 0, sizeof(struct mp3_task_struct));
      task->linux_task = find_task_by_pid(pid);
      task->pid = pid;

      struct process_list* node = kmalloc(sizeof(struct process_list), GFP_KERNEL);
      memset(node, 0, sizeof(struct process_list));
      node->mp3_task = task;
      INIT_LIST_HEAD(&(node->list));

      spin_lock_irq(&list_lock);
      int was_empty = list_empty(&(registered_processes->list));
      list_add_tail(&(node->list), &(registered_processes->list));

      // create work/job if first pid registered
      if (was_empty) { 
         work = kmalloc(sizeof(struct delayed_work), GFP_KERNEL);
         INIT_DELAYED_WORK(work, work_callback);
         queue_delayed_work(queue, work, msecs_to_jiffies(50));
      }
      spin_unlock_irq(&list_lock);
   }
   else if (operation == 'U') {
      // delete process from linked list
      struct process_list* tmp;
      struct list_head *curr_pos, *q;

      spin_lock_irq(&list_lock);
      list_for_each_safe(curr_pos, q, &(registered_processes->list)) {
         tmp = list_entry(curr_pos, struct process_list, list);
         
         if (tmp->mp3_task->pid == pid) {
            list_del(curr_pos);
            kfree(tmp);
            //printk("MP3 deleting pid %d from list\n", pid);
         }
      }
      // if list is empty, job is also deleted
      if (list_empty(&(registered_processes->list))) { 
         //printk("MP3 list emptied\n");
         cancel_delayed_work_sync(work);
         kfree(work);
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

// map physical pages of shared_mem_buffer to virtual address space
static int _char_dev_mmap_callback(struct file* filep, struct vm_area_struct* vma) {
   int i = 0;
   unsigned long user_process_msize = vma->vm_end - vma->vm_start;
   //printk("MP3 len = %lu\n", user_process_msize);

   for (; i < user_process_msize; i += BUF_PAGE_SIZE) {
      // get physical page address of virtual page in buffer
      unsigned long pfn = vmalloc_to_pfn((char*)shared_mem_buffer + i); 

      // map continguous physical address space to the virtual space
      int ret = remap_pfn_range(vma, vma->vm_start + i, pfn, BUF_PAGE_SIZE, vma->vm_page_prot);
      if (ret < 0) {
         printk("MP3 remap_pfn_range didn't work");
         return -EIO;
      }
   }
   printk("MP3 finished mmap()\n");
   return 0;
}

const struct file_operations dev_fops = {
   .open = NULL,
   .mmap = _char_dev_mmap_callback,
   .release = NULL,
   .owner = THIS_MODULE
};

// mp1_init - Called when module is loaded
int __init mp3_init(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE LOADING\n");
   #endif
   
   proc_dir = proc_mkdir("mp3", NULL);
   proc_file = proc_create("status", 0666, proc_dir, &proc_fops);
   
   registered_processes = kmalloc(sizeof(struct process_list), GFP_KERNEL);
   INIT_LIST_HEAD(&(registered_processes->list));

   spin_lock_init(&list_lock);
   spin_lock_init(&buffer_lock);

   queue = create_workqueue("queue");

   shared_mem_buffer = vmalloc(BUF_PAGE_SIZE * BUF_NUM_PAGES);
   buffer_pos = 0;
   
   register_chrdev_region(MKDEV(423, 0), 1, "mp3dev");
   cdev = kmalloc(sizeof(struct cdev), GFP_KERNEL);
   cdev_init(cdev, &dev_fops);
   cdev_add(cdev, MKDEV(423, 0), 1);
   
   printk(KERN_ALERT "MP3 MODULE LOADED\n");
   return 0;   
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif

   flush_workqueue(queue);
   destroy_workqueue(queue);

   struct process_list* tmp;
   struct list_head *pos, *q;
   list_for_each_safe(pos, q, &(registered_processes->list)) {
      tmp = list_entry(pos, struct process_list, list);
      list_del(pos);
      kfree(tmp);
   }

   vfree(shared_mem_buffer);

   unregister_chrdev_region(MKDEV(423, 0), 1);
   cdev_del(cdev);
   
   remove_proc_entry("status", proc_dir);
   remove_proc_entry("mp3", NULL);

   printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
