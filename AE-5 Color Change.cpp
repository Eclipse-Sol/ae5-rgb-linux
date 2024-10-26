#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstdint>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>
#include <map>

// Hardware configuration constants
#define MMIO_REGION_SIZE 0x1024
#define LED_CONTROL_OFFSET 0x320
#define NUM_LEDS 5

// LED protocol constants
#define START_FRAME_BITS 32
#define BRIGHTNESS_BITS 8
#define COLOR_BITS 24
#define WRITE_ITERATIONS 2

// LED protocol values
#define LED_BIT_LOW 0x02
#define LED_BIT_HIGH 0x102
#define LED_CLOCK_HIGH 0x103
#define LED_CLOCK_LOW 0x03

// PCI device identification
#define TARGET_VENDOR "1102"
#define TARGET_DEVICE "0012"
#define TARGET_REGION 2

// Type definition for MMIO register access
typedef volatile uint32_t* mmio_reg_t;

/**
 * Structure to represent RGB color values
 * Each component ranges from 0-255
 */
struct RGB {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    RGB(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) : red(r), green(g), blue(b) {}
};

// LED color configuration structure
struct LEDConfig {
    int position;
    RGB color;
    
    LEDConfig(int pos, const RGB& c) : position(pos), color(c) {}
};

// Function declarations
void print_usage(const char* program_name);
bool parse_led_configs(int argc, char* argv[], std::vector<LEDConfig>& configs);
bool parse_color(const char* str, RGB& color);
bool parse_single_color(int argc, char* argv[], RGB& color);

class ScopedFD {
public:
    explicit ScopedFD(int fd) : fd_(fd) {}
    ~ScopedFD() { if (fd_ >= 0) close(fd_); }
    int get() const { return fd_; }
    int release() { int tmp = fd_; fd_ = -1; return tmp; }
private:
    int fd_;
    // Prevent copying
    ScopedFD(const ScopedFD&) = delete;
    ScopedFD& operator=(const ScopedFD&) = delete;
};

class ScopedMMIO {
public:
    ScopedMMIO(void* base, size_t size) : base_(base), size_(size) {}
    ~ScopedMMIO() { 
        if (base_ != MAP_FAILED) {
            munmap(base_, size_);
        }
    }
    void* get() const { return base_; }
private:
    void* base_;
    size_t size_;
    // Prevent copying
    ScopedMMIO(const ScopedMMIO&) = delete;
    ScopedMMIO& operator=(const ScopedMMIO&) = delete;
};

bool check_root_privileges() {
    return (geteuid() == 0);
}

void write_mmio(void* base, uint32_t offset, uint32_t value) {
    mmio_reg_t reg = (mmio_reg_t)((uint8_t*)base + offset);
    *reg = value;
}

// Function to find MMIO base address
uint64_t find_mmio_base_address() {
    const char* pci_path = "/sys/bus/pci/devices";
    DIR* dir = opendir(pci_path);
    if (!dir) {
        throw std::runtime_error("Failed to open PCI devices directory");
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string device_path = std::string(pci_path) + "/" + entry->d_name;

        // Read vendor ID
        std::ifstream vendor_file(device_path + "/vendor");
        std::string vendor_id;
        if (!vendor_file || !(vendor_file >> vendor_id)) continue;
        vendor_id = vendor_id.substr(2); // Remove "0x" prefix

        // Read device ID
        std::ifstream device_file(device_path + "/device");
        std::string device_id;
        if (!device_file || !(device_file >> device_id)) continue;
        device_id = device_id.substr(2); // Remove "0x" prefix

        // Check if this is our target device
        if (vendor_id == TARGET_VENDOR && device_id == TARGET_DEVICE) {
            // Read resource file to get memory regions
            std::ifstream resource_file(device_path + "/resource");
            std::string line;
            int region = 0;

            while (std::getline(resource_file, line) && region <= TARGET_REGION) {
                if (region == TARGET_REGION) {
                    uint64_t start, end;
                    if (sscanf(line.c_str(), "0x%lx 0x%lx", &start, &end) == 2) {
                        closedir(dir);
                        return start;
                    }
                }
                region++;
            }
        }
    }

    closedir(dir);
    throw std::runtime_error("Failed to find target device or memory region");
}

void write_led_bit(void* base, bool is_high) {
    write_mmio(base, LED_CONTROL_OFFSET, is_high ? LED_BIT_HIGH : LED_BIT_LOW);
    write_mmio(base, LED_CONTROL_OFFSET, LED_CLOCK_HIGH);
    write_mmio(base, LED_CONTROL_OFFSET, LED_CLOCK_LOW);
}

uint32_t rgb_to_hex(const RGB& color) {
    return (color.blue << 16) | (color.green << 8) | color.red;
}


void print_usage(const char* program_name) {
    fprintf(stderr, "AE-5 RGB Controller\n");
    fprintf(stderr, "===================================\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  Single color for all LEDs:\n");
    fprintf(stderr, "    %s <r> <g> <b>\n\n", program_name);
    fprintf(stderr, "  Different colors per LED:\n");
    fprintf(stderr, "    %s <led_position>:<r,g,b> [<led_position>:<r,g,b> ...]\n\n", program_name);
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  led_position : LED number (0-%d)\n", NUM_LEDS-1);
    fprintf(stderr, "  r,g,b       : RGB values (0-255)\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s 255 0 0               # Set all LEDs to red\n", program_name);
    fprintf(stderr, "  %s 0:255,0,0             # Set LED 0 to red\n", program_name);
    fprintf(stderr, "  %s 0:255,0,0 1:0,255,0   # Set LED 0 to red, LED 1 to green\n", program_name);
    fprintf(stderr, "\nNote: This program requires root privileges to access hardware.\n");
    fprintf(stderr, "Run with sudo or as root user.\n");
}

bool parse_color(const char* str, RGB& color) {
    int r, g, b;
    if (sscanf(str, "%d,%d,%d", &r, &g, &b) != 3) {
        return false;
    }
    
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        return false;
    }
    
    color = RGB(r, g, b);
    return true;
}

bool parse_single_color(int argc, char* argv[], RGB& color) {
    if (argc != 4) return false;
    
    char* endptr;
    errno = 0;
    
    // Parse R, G, B values
    long r = strtol(argv[1], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || r < 0 || r > 255) return false;
    
    long g = strtol(argv[2], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || g < 0 || g > 255) return false;
    
    long b = strtol(argv[3], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || b < 0 || b > 255) return false;
    
    color = RGB(r, g, b);
    return true;
}

bool parse_led_configs(int argc, char* argv[], std::vector<LEDConfig>& configs) {
    // First try to parse as a single color for all LEDs
    RGB single_color;
    if (parse_single_color(argc, argv, single_color)) {
        // Add the same color for all LEDs
        for (int i = 0; i < NUM_LEDS; i++) {
            configs.emplace_back(i, single_color);
        }
        return true;
    }

    // If not a single color, parse as individual LED configurations
    for (int i = 1; i < argc; i++) {
        char* pos_str = strtok(argv[i], ":");
        char* color_str = strtok(nullptr, ":");
        
        if (!pos_str || !color_str) {
            fprintf(stderr, "Error: Invalid format for LED configuration: %s\n", argv[i]);
            fprintf(stderr, "Expected format: <position>:<r,g,b> or <r> <g> <b>\n");
            return false;
        }
        
        // Parse LED position
        char* endptr;
        errno = 0;
        long pos = strtol(pos_str, &endptr, 10);
        if (errno != 0 || *endptr != '\0' || pos < 0 || pos >= NUM_LEDS) {
            fprintf(stderr, "Error: Invalid LED position: %s\n", pos_str);
            return false;
        }
        
        // Parse color
        RGB color;
        if (!parse_color(color_str, color)) {
            fprintf(stderr, "Error: Invalid color format: %s\n", color_str);
            fprintf(stderr, "Expected format: r,g,b (0-255)\n");
            return false;
        }
        
        configs.emplace_back(pos, color);
    }
    
    return true;
}

void send_start_frame(void* mmio_base) {
    for (int i = 0; i < START_FRAME_BITS; i++) {
        write_led_bit(mmio_base, false);
    }
}

void send_led_color(void* mmio_base, uint32_t color_value) {
    // Send brightness bits (all 1's for maximum brightness)
    for (int i = 0; i < BRIGHTNESS_BITS; i++) {
        write_led_bit(mmio_base, true);
    }

    // Send color bits
    for (int i = 0; i < COLOR_BITS; i++) {
        uint32_t bit = (color_value >> (23 - i)) & 0x01;
        write_led_bit(mmio_base, bit == 1);
    }
}

void send_end_frame(void* mmio_base) {
    for (int i = 0; i < 32; i++) {
        write_led_bit(mmio_base, true);
    }
}

int main(int argc, char* argv[]) {
    if (!check_root_privileges()) {
        fprintf(stderr, "Error: This program requires root privileges to access hardware.\n");
        fprintf(stderr, "Please run with sudo or as root user.\n");
        return 1;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Parse LED configurations
    std::vector<LEDConfig> led_configs;
    if (!parse_led_configs(argc, argv, led_configs)) {
        return 1;
    }

    // Create a map of LED positions to colors
    std::map<int, RGB> led_colors;
    for (const auto& config : led_configs) {
        if (led_colors.count(config.position) > 0) {
            fprintf(stderr, "Warning: Multiple colors specified for LED %d. Using the last one.\n", 
                    config.position);
        }
        led_colors[config.position] = config.color;
    }

    // Open the device with RAII
    ScopedFD fd(open("/dev/mem", O_RDWR | O_SYNC));
    if (fd.get() < 0) {
        perror("Failed to open /dev/mem");
        return 1;
    }

    // Find MMIO base address
    uint64_t mmio_base_addr;
    try {
        mmio_base_addr = find_mmio_base_address();
    } catch (const std::runtime_error& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    // Map MMIO region with RAII
    void* mmio_base = mmap(NULL, MMIO_REGION_SIZE, PROT_READ | PROT_WRITE, 
                          MAP_SHARED, fd.get(), mmio_base_addr);
    if (mmio_base == MAP_FAILED) {
        perror("Failed to map MMIO region");
        return 1;
    }
    ScopedMMIO mmio(mmio_base, MMIO_REGION_SIZE);

    // Send LED data
    send_start_frame(mmio.get());

    // Send color data for each LED
    for (int led = 0; led < NUM_LEDS; led++) {
        uint32_t color_value = 0;
        if (led_colors.count(led) > 0) {
            color_value = rgb_to_hex(led_colors[led]);
        }
        send_led_color(mmio.get(), color_value);
    }

    send_end_frame(mmio.get());

    return 0;
}