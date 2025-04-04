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
#define MYTRAFFIC_MAJOR 61

/* ======================= Global variables ======================= */
unsigned int btn_0_irq; // IRQ number for button 0
unsigned int btn_1_irq; // IRQ number for button 1
typedef enum {
    NORMAL_MODE,
    FLASHING_RED,
    FLASHING_YELLOW,
    PEDESTRIAN_MODE,
    LIGHTBULB_CHECK
} opmode_t;

typedef enum {
    EVENT_BTN_0_PRESS,
    EVENT_BTN_1_PRESS,
    EVENT_BOTH_BTNS_PRESS,
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

traffic_light_t *light; // pointer to traffic light struct, global for read/write access

opmode_t state_transition_table[4][5] = { // current mode vs. event
                        /* NORMAL_MODE       FLASHING_RED      FLASHING_YELLOW      PEDESTRIAN_MODE     LIGHTBULB_CHECK*/
    /* EVENT_BTN_0_PRESS */ {FLASHING_RED,   FLASHING_YELLOW,    NORMAL_MODE,   PEDESTRIAN_MODE,    NORMAL_MODE},
    /* EVENT_BTN_1_PRESS */ {PEDESTRIAN_MODE,  FLASHING_RED,  FLASHING_YELLOW,   PEDESTRIAN_MODE,   NORMAL_MODE}, // only go to pedestrian mode from normal
    /* EVENT_BOTH_BTNS_PRESS */ {LIGHTBULB_CHECK,   LIGHTBULB_CHECK,    LIGHTBULB_CHECK,    LIGHTBULB_CHECK,    LIGHTBULB_CHECK},
    /* EVENT_TIMER_EXPIRE */ {NORMAL_MODE,   FLASHING_RED,   FLASHING_YELLOW,   NORMAL_MODE,    LIGHTBULB_CHECK} // pedestrian mode will return to normal after timer expires, lightbulb check ignores any existing timers/their expirations
};

/* ======================= Function Declarations/Definitions ======================= */
static int gpio_init(traffic_light_t *light); // GPIO and IRQ initialization function
void set_light_status(traffic_light_t *light); // helper function to set GPIOs based on light status

// state handlers
void handle_normal_mode(traffic_light_t *light) {
    printk(KERN_INFO "Handling normal mode\n"); // temp
    if (light->status.green) {
        light->status.green = false;
        light->status.yellow = true;
        mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate)); // yellow for 1 cycle
    } else if (light->status.yellow && !light->pedestrian_present) { // switch to red only if no pedestrian is present
        light->status.yellow = false;
        light->status.red = true;
        mod_timer(&light->timer, jiffies + (2 * HZ / light->cycle_rate)); // red for 2 cycles
    } else if (light->status.red) {
        light->status.red = false;
        light->status.green = true;
        mod_timer(&light->timer, jiffies + (3 * HZ / light->cycle_rate)); // green for 3 cycles
    } else if (!light->status.red && !light->status.yellow && !light->status.green) { // all lights are off when switching modes
        light->status.green = true; // default to green
        mod_timer(&light->timer, jiffies + (3 * HZ / light->cycle_rate));
    }
    set_light_status(light); // update GPIOs based on current light status
}

void handle_flashing_red(traffic_light_t *light) {
    printk(KERN_INFO "Handling flashing red mode\n"); // temp
    light->status.red = !light->status.red; // toggle red light
    light->status.yellow = false;
    light->status.green = false;
    mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate));
    set_light_status(light);
}

void handle_flashing_yellow(traffic_light_t *light) {
    printk(KERN_INFO "Handling flashing yellow mode\n"); // temp
    light->status.yellow = !light->status.yellow; // toggle yellow light
    light->status.red = false;
    light->status.green = false;
    mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate));
    set_light_status(light);
}

void handle_pedestrian_mode(traffic_light_t *light) {
    // if in pedestrian mode & red light is on, keep red and yellow on for 5 cycles instead of 2 cycles
    // otherwise, resume normal mode (after timer expiration) until stop phase (red light on) in reached
    printk(KERN_INFO "Handling pedestrian mode\n"); // temp
    if (light->status.yellow) {
        light->status.red = true;
        light->status.green = false;
        mod_timer(&light->timer, jiffies + (5 * HZ / light->cycle_rate)); // red/yellow for 5 cycles
        set_light_status(light); // update GPIOs based on current light status
    }
    // else, let current timer expire to return to normal mode
}

void handle_lightbulb_check(traffic_light_t *light) {
    // turn on all lights for lightbulb check
    light->status.red = true;
    light->status.yellow = true;
    light->status.green = true;
    set_light_status(light);
    if (!gpio_get_value(BTN_0) && !gpio_get_value(BTN_1)) { // if both buttons are released
        light->cycle_rate = 1; // reset cycle rate to 1 Hz
        light->status.red = false;
        light->status.yellow = false;
        light->mode = NORMAL_MODE; // reset mode to normal
        mod_timer(&light->timer, jiffies + (3 * HZ / light->cycle_rate)); // reset timer for normal mode
        set_light_status(light); // update lights
        return;
    }
    mod_timer(&light->timer, jiffies + msecs_to_jiffies(10)); // set timer to check every 10 ms for button release
}
void handle_event(traffic_light_t *light, event_t event) {
    opmode_t next_mode = state_transition_table[event][light->mode]; // get next mode based on current mode and event
    
    // for pedestrian mode
    if (light->pedestrian_present && light->status.yellow && !light->status.red) {
        next_mode = PEDESTRIAN_MODE; // if pedestrian present & and about to enter "stop" phase, force to pedestrian mode
    }

    if (light->pedestrian_present && light->status.red && light->status.yellow) {
        light->status.red = false; // reset red & yellow lights for return to normal mode
        light->status.yellow = false;
        light->pedestrian_present = false; // clear pedestrian present flag
    }
    light->mode = next_mode; // update mode

    if (event == EVENT_BTN_1_PRESS && (light->mode == FLASHING_RED || light->mode == FLASHING_YELLOW)) {
        // if pedestrian button is pressed while in flashing mode, don't do anything (don't call handler again) to prevent light jittering
        return;
    }

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
        case PEDESTRIAN_MODE:
            light->pedestrian_present = true; // set pedestrian present flag
            handle_pedestrian_mode(light);
            break;
        case LIGHTBULB_CHECK:
            light->pedestrian_present = false; // clear pedestrian present flag
            handle_lightbulb_check(light);
            break;
    }
};

static unsigned long last_btn_0_irq_time = 0;

static irqreturn_t btn_0_irq_handler(int irq, void *dev_id) {
    // handle mode switch button press (BTN0)
    unsigned long current_time = jiffies;

    // button debounce (ignore interrupts occurring within 50ms of each other)
    if (current_time < last_btn_0_irq_time + msecs_to_jiffies(50)) {
        return IRQ_HANDLED;
    }
    last_btn_0_irq_time = current_time;

    if (gpio_get_value(BTN_1)) { // check if BTN1 is pressed
        handle_event(light, EVENT_BOTH_BTNS_PRESS); // both buttons pressed
    } else {
        handle_event(light, EVENT_BTN_0_PRESS); // only BTN0 pressed
    }
    return IRQ_HANDLED;
}

static unsigned long last_btn_1_irq_time = 0;

static irqreturn_t btn_1_irq_handler(int irq, void *dev_id) {
    // handle pedestrian call button press (BTN1)
    unsigned long current_time = jiffies;

    // button debounce
    if (current_time < last_btn_1_irq_time + msecs_to_jiffies(50)) {
        return IRQ_HANDLED;
    }
    last_btn_1_irq_time = current_time;

    if (gpio_get_value(BTN_0)) { // check if BTN0 is pressed
        handle_event(light, EVENT_BOTH_BTNS_PRESS); // both buttons pressed
    } else {
        handle_event(light, EVENT_BTN_1_PRESS); // only BTN1 pressed
    }
    return IRQ_HANDLED;
}

static void mytraffic_timer_callback(struct timer_list *t) {
    handle_event(light, EVENT_TIMER_EXPIRE);
}

static ssize_t mytraffic_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
    char tbuf[256], *tbptr = tbuf;
    size_t len; 

    if (*f_pos > 0) {
        return 0; // no more data to read
    }

    // print current mode, cycle rate, light status, and pedestrian presence to kernel buffer
    tbptr += sprintf(tbptr, "Operational mode: %s\n",
        light->mode == NORMAL_MODE ? "normal" : 
        light->mode == FLASHING_RED ? "flashing-red" : 
        light->mode == FLASHING_YELLOW ? "flashing-yellow" : 
        light->mode == PEDESTRIAN_MODE ? "pedestrian-mode" : "lightbulb-check");
    tbptr += sprintf(tbptr, "Cycle rate: %d Hz\n", light->cycle_rate);
    tbptr += sprintf(tbptr, "Red status: %s\n", light->status.red ? "on" : "off");
    tbptr += sprintf(tbptr, "Yellow status: %s\n", light->status.yellow ? "on" : "off");
    tbptr += sprintf(tbptr, "Green status: %s\n", light->status.green ? "on" : "off");
    tbptr += sprintf(tbptr, "Pedestrian present?: %s\n", light->pedestrian_present ? "yes" : "no");

    len = tbptr - tbuf; // length of string in temporary buffer

    // limit count to prevent buffer overflows
    if (count > len - *f_pos) {
        count = len - *f_pos;
    }

    // copy to user, check for errors
    if (copy_to_user(buf, tbuf + *f_pos, count)) {
        return -EFAULT;
    }

    *f_pos += count; // increment file position
    return count;
}

static ssize_t mytraffic_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos) {
    char kbuf[256];
    int new_rate;

    if (count > sizeof(kbuf) - 1) {
        return -1;
    }

    if (copy_from_user(kbuf, buf, count)) {
        return -EFAULT;
    }

    kbuf[count] = '\0'; // null terminate string

    if (sscanf(kbuf, "%d", &new_rate) == 1) { // check for valid input
        if (new_rate < 1 || new_rate > 9) {
            return -1; // invalid cycle rate
        } else {
            light->cycle_rate = new_rate; // set new cycle rate
            // mod_timer(&light->timer, jiffies + (HZ / light->cycle_rate)); // reset timer with new cycle rate
            return count;
        }
    } 

    // otherwise invalid input
    return -1;
}

static struct file_operations mytraffic_fops = {
	.owner = THIS_MODULE,
	.read = mytraffic_read,
	.write = mytraffic_write
};

static int mytraffic_init(void) {
    // register char device
    int result;
    result = register_chrdev(MYTRAFFIC_MAJOR, "mytraffic", &mytraffic_fops);
    if (result < 0) {
        printk(KERN_ERR "Failed to register char device\n");
        return result;
    }

    light = kmalloc(sizeof(traffic_light_t), GFP_KERNEL); // allocate memory for traffic light struct
    if (!light) {
        printk(KERN_ERR "Failed to allocate memory for traffic light struct\n");
        kfree(light);
        unregister_chrdev(MYTRAFFIC_MAJOR, "mytraffic");
        return -ENOMEM;
    }
    
    // set up GPIOs
    if (gpio_init(light) < 0) {
        printk(KERN_ERR "Failed to initialize GPIOs\n");
        kfree(light);
        unregister_chrdev(MYTRAFFIC_MAJOR, "mytraffic");
        return -1;
    }

    // initialize traffic light struct
    light->mode = NORMAL_MODE; // start in normal mode
    light->cycle_rate = 1; // default cycle rate (1 Hz)
    light->status.red = true; // start with red light "on" to trigger green
    light->status.yellow = false;
    light->status.green = false; // 
    light->pedestrian_present = false; // no pedestrian by default
    timer_setup(&light->timer, mytraffic_timer_callback, 0); // initialize timer with callback
    mod_timer(&light->timer, jiffies + (2 * HZ / light->cycle_rate)); // start the timer

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
    
    // free timer
    del_timer_sync(&light->timer); // ensure timer is fully stopped

    // free traffic light struct
    kfree(light);

    // unregister char device
    unregister_chrdev(MYTRAFFIC_MAJOR, "mytraffic");
}

static int gpio_init(traffic_light_t *light) {
    int result = 0; // for error checking

    if (!light) {
        printk(KERN_ERR "Invalid traffic light pointer\n");
        return -1;
    }
    
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