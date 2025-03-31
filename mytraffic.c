/*
	3 Operational Modes:
		1. Normal Mode: 
			- Green for 3 cycless
			- Yellow for 1 cycle
			- Red for 2 cycles
		2. Flashing Red
			- Red for 1 cycle
			- Off for 1 cycle
		3. Flashing Yellow
			- Yellow for 1 cycle
			- Off for 1 cycle

	Read from character device (61, 0) at /dev/mytraffic:
		- Current mode
		- Current cycle rate (Hz)
		- Current status of each light (Red off, Yellow off, Green on)
		- Pedestrian present? (Currently crossing/waiting to cross after pressing cross button)

	Write to character device:
		- Write int (1-9) sets the cycle rate 
			- Ex: echo 2 > /dev/mytraffic sets cycle rate to 2 Hz, so each cycle is 0.5 seconds
		- Ignore any other data written

	Pedestrian Call Button (BTN_1):
		- For normal mode
		- At the next stop phase (red), turn on both red and yellow for 5 cycles instead of red for 2 cycles
		- Return to normal after 

	Lightbulb check feature:
		- Hold both buttons: ON all lights
		- Release: Reset to initial state (normal mode, 1 Hz cycle rate, 3 cycles green, no pedestrians)

*/

/*
	GPIO Pins:
		Red light: 67
		Yellow light: 68
		Green light: 44
		Button 0: 26
		Button 1: 46
		
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/fs.h>			// filesystem operations
#include <linux/uaccess.h>		// copy_to/from_user
#include <linux/gpio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Traffic light kernel module");

#define RED 67
#define YELLOW 68
#define GREEN 44
#define BTN_0 26	// Mode switch button
#define BTN_1 46	// Pedestrian call button
#define MAJOR 61

/* ======================= Global variables ======================= */


/* =======================  ======================= */
static irqreturn_t btn_0_irq_handler(int irq, void *dev_id) {

}

static irqreturn_t btn_1_irq_handler(int irq, void *dev_id) {

}

static void mytraffic_timer_callback(struct timer_lists *t) {

}

static ssize_t mytraffic_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {

}

static ssize_t mytraffic_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {

}

static struct file_operations mytraffic_fops = {
	.owner = THIS_MODULE,
	.read = mytraffic_read,
	.write = mytraffic_write,
}

static int __init mytraffic_init(void) {

}

static int __exit mytraffic_exit(void) {

}

module_init(mytraffic_init);
module_exit(mytraffic_exit);