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
unsigned int btn_0_irq; // IRQ number for button 0
unsigned int btn_1_irq; // IRQ number for button 1

typedef enum {
    NORMAL_MODE,
    FLASHING_RED,
    FLASHING_YELLOW
} opmode_t;

typedef enum {
    EVENT_BTN_0_PRESS,
    // EVENT_BTN_1_PRESS,
    // EVENT_BOTH_BUTTONS_PRESS,
    EVENT_TIMER_EXPIRE
} event_t;

typedef struct {
    bool red;   // 0 = off, 1 = on
    bool yellow;    
    bool green;
} light_status_t;
typedef struct {
    struct timer_list timer; // timer for traffic light cycles
    opmode_t mode; // current operational mode
    light_status_t status; // current status of each light
    int cycle_rate; // in Hz
    bool pedestrian_present;
} traffic_light_t;

opmode_t state_transition_table[2][3] = { // current mode vs. event
                        /* NORMAL_MODE       FLASHING_RED      FLASHING_YELLOW */
    /* EVENT_BTN_0_PRESS */ {FLASHING_RED,   FLASHING_YELLOW,    NORMAL_MODE},
    /* EVENT_TIMER_EXPIRE */ {NORMAL_MODE,   FLASHING_RED,   FLASHING_YELLOW}
};

/* ======================= Function Declarations/Definitions ======================= */
static int gpio_init(traffic_light_t *light); // GPIO and IRQ initialization function
void set_light_status(traffic_light_t *light); // helper function to set GPIOs based on light status

// state handlers
void handle_normal_mode(traffic_light_t *light) {
    if (light->status.green) {
        light->status.green = false;
        light->status.yellow = true;
        mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate)); // yellow for 1 cycle
    } else if (light->status.yellow) {
        light->status.yellow = false;
        light->status.red = true;
        mod_timer(&light->timer, jiffies + (2 * HZ / light->cycle_rate)); // red for 2 cycles
    } else if (light->status.red) {
        light->status.red = false;
        light->status.green = true;
        mod_timer(&light->timer, jiffies + (3 * HZ / light->cycle_rate)); // green for 3 cycles
    }
    set_light_status(light); // update GPIOs based on current light status
}

void handle_flashing_red(traffic_light_t *light) {
    light->status.red = !light->status.red; // toggle red light
    light->status.yellow = false;
    light->status.green = false;
    mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate));
    set_light_status(light);
}

void handle_flashing_yellow(traffic_light_t *light) {
    light->status.yellow = !light->status.yellow; // toggle yellow light
    light->status.red = false;
    light->status.green = false;
    mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate));
    set_light_status(light);
}

void handle_lightbulb_check(traffic_light_t *light) {
    // turn on all lights for lightbulb check
    light->status.red = true;
    light->status.yellow = true;
    light->status.green = true;
    mod_timer(&light->timer, jiffies + (5 * HZ / light->cycle_rate)); // 5 cycles for lightbulb check
    set_light_status(light);
}
void handle_event(traffic_light_t *light, event_t event) {
    opmode_t next_mode = state_transition_table[light->mode][event]; // get next mode based on current mode and event
    light->mode = next_mode; // update mode
    switch (next_mode) {
        case NORMAL_MODE:
            handle_normal_mode(light);
            break;
        case FLASHING_RED:
            handle_flashing_red(light);
            break;
        case FLASHING_YELLOW:
            handle_flashing_yellow(light);
            break;
    }
};

static irqreturn_t btn_0_irq_handler(int irq, void *dev_id) {
    // handle mode switch button press (BTN0)
    traffic_light_t *light = (traffic_light_t *)dev_id; // get light status from dev_id
    handle_event(light, EVENT_BTN_0_PRESS);
    return IRQ_HANDLED;
}

static irqreturn_t btn_1_irq_handler(int irq, void *dev_id) {
    // handle pedestrian call button press (BTN1)
    traffic_light_t *light = (traffic_light_t *)dev_id; // get light status from dev_id
    // handle_event(light, EVENT_BTN_1_PRESS);
    return IRQ_HANDLED;
}

static void mytraffic_timer_callback(struct timer_list *t) {
    traffic_light_t *light = from_timer(light, t, timer);
    handle_event(light, EVENT_TIMER_EXPIRE);
}

static ssize_t mytraffic_read(struct file *file, char __user *buf, size_t count, loff_t *offset) {

}

static ssize_t mytraffic_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {

}

static struct file_operations mytraffic_fops = {
	.owner = THIS_MODULE,
	.read = mytraffic_read,
	.write = mytraffic_write
};

static int mytraffic_init(void) {
    // register char device
    int result;
    result = register_chrdev(MAJOR, "mytraffic", &mytraffic_fops);
    if (result < 0) {
        printk(KERN_ERR "Failed to register char device\n");
        return result;
    }

    traffic_light_t *light = kmalloc(sizeof(traffic_light_t), GFP_KERNEL); // allocate memory for traffic light struct
    if (!light) {
        printk(KERN_ERR "Failed to allocate memory for traffic light struct\n");
        kfree(light);
        unregister_chrdev(MAJOR, "mytraffic");
        return -ENOMEM;
    }
    
    // set up GPIOs
    if (gpio_init(light) < 0) {
        printk(KERN_ERR "Failed to initialize GPIOs\n");
        kfree(light);
        unregister_chrdev(MAJOR, "mytraffic");
        return -1;
    }

    // initialize traffic light struct
    light->mode = NORMAL_MODE; // start in normal mode
    light->cycle_rate = 1; // default cycle rate (1 Hz)
    light->status.red = false; // start with all lights off
    light->status.yellow = false;
    light->status.green = true; // start with green on (normal mode)
    light->pedestrian_present = false; // no pedestrian by default
    timer_setup(&light->timer, mytraffic_timer_callback, 0); // initialize timer with callback
    mod_timer(&light->timer, jiffies + (3 * HZ / light->cycle_rate)); // start the timer for the first instance of normal mode (3 cycles of green)

    return 0;
}

static void mytraffic_exit(void) {
    // free IRQs and GPIOs
    free_irq(btn_1_irq, NULL);
    free_irq(btn_0_irq, NULL);
    gpio_free(BTN_1);
    gpio_free(BTN_0);
    gpio_free(GREEN);
    gpio_free(YELLOW);
    gpio_free(RED);

    // unregister char device
    unregister_chrdev(MAJOR, "mytraffic");
}

static int gpio_init(traffic_light_t *light) {
    if (!light) {
        printk(KERN_ERR "Invalid traffic light pointer\n");
        return -1;
    }
    
    int result = 0; // for error checking

    // set up RED GPIO
    if (gpio_request(RED, "RED")) {
        printk(KERN_ERR "Failed to allocate GPIO %d\n", RED);
        result = -1;
    }
    // set RED direction to output
    if (gpio_direction_output(RED, 0)) {
        printk(KERN_ERR "Failed to set GPIO %d direction\n", RED);
        result = -1;
    }

    // set up YELLOW GPIO
    if (gpio_request(YELLOW, "YELLOW")) {
        printk(KERN_ERR "Failed to allocate GPIO %d\n", YELLOW);
        result = -1;
    }
    // set YELLOW direction to output
    if (gpio_direction_output(YELLOW, 0)) {
        printk(KERN_ERR "Failed to set GPIO %d direction\n", YELLOW);
        result = -1;
    }
    // set up GREEN GPIO
    if (gpio_request(GREEN, "GREEN")) {
        printk(KERN_ERR "Failed to allocate GPIO %d\n", GREEN);
        result = -1;
    }
    // set GREEN direction to output
    if (gpio_direction_output(GREEN, 0)) { 
        printk(KERN_ERR "Failed to set GPIO %d direction\n", GREEN);
        result = -1;
    }

    // set up BTN_0 GPIO
    if (gpio_request(BTN_0, "BTN_0")) {
        printk(KERN_ERR "Failed to allocate GPIO %d\n", BTN_0);
        result = -1;
    }
    // set BTN_0 direction to input
    if (gpio_direction_input(BTN_0)) {
        printk(KERN_ERR "Failed to set GPIO %d direction\n", BTN_0);
        result = -1;
    }
    // set up BTN_0 IRQ
    btn_0_irq = gpio_to_irq(BTN_0);
    if (request_irq(btn_0_irq, btn_0_irq_handler, IRQF_TRIGGER_RISING, "btn_0_irq", light) != 0) {
        printk(KERN_ERR "Failed to request IRQ %d\n", btn_0_irq);
        result = -1;
    }

    // set up BTN_1 GPIO
    if (gpio_request(BTN_1, "BTN_1")) {
        printk(KERN_ERR "Failed to allocate GPIO %d\n", BTN_1);
        result = -1;
    }
    // set BTN_1 direction to input
    if (gpio_direction_input(BTN_1)) {
        printk(KERN_ERR "Failed to set GPIO %d direction\n", BTN_1);
        result = -1;
    }
    // set up BTN_1 IRQ
    btn_1_irq = gpio_to_irq(BTN_1);
    if (request_irq(btn_1_irq, btn_1_irq_handler, IRQF_TRIGGER_RISING, "btn_1_irq", light) != 0) {
        printk(KERN_ERR "Failed to request IRQ %d\n", btn_1_irq);
        result = -1;
    }

    if (result < 0) {
        // free GPIOs and IRQs in case of error
        free_irq(btn_1_irq, NULL);
        free_irq(btn_0_irq, NULL);
        gpio_free(BTN_1);
        gpio_free(BTN_0);
        gpio_free(GREEN);
        gpio_free(YELLOW);
        gpio_free(RED);
    }
    return result;
}

void set_light_status(traffic_light_t *light) {
    gpio_set_value(RED, light->status.red ? 1 : 0);
    gpio_set_value(YELLOW, light->status.yellow ? 1 : 0);
    gpio_set_value(GREEN, light->status.green ? 1 : 0);

}

module_init(mytraffic_init);
module_exit(mytraffic_exit);