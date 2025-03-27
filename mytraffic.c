#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h> /* kmalloc() */
#include <linux/fs.h> /* filesystem operations */
#include <linux/timer.h> /* timer functionality */
#include <linux/uaccess.h> /* copy_to/from_user */

MODULE_LICENSE("Dual BSD/GPL");

#define MAX_TIMERS 1
#define MYTIMER_MAJOR 61
