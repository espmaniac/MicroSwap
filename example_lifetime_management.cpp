/**
 * @file example_lifetime_management.cpp
 * @brief Example demonstrating VMArray and VMPtr improvements
 * 
 * This example shows how the new lifetime management features work
 * for both trivial and non-trivial types in an Arduino-compatible way.
 */

// This example would be used in an Arduino environment
// For demonstration purposes, we show the key concepts

#include "containers.h"

// Example 1: Non-trivial class with constructor/destructor
class Sensor {
private:
    int pin;
    bool initialized;
    
public:
    // Constructor is called automatically when stored in VMArray
    Sensor() : pin(-1), initialized(false) {
        Serial.println("Sensor() constructor called");
    }
    
    // Destructor is called automatically when VMArray is destroyed
    ~Sensor() {
        Serial.println("~Sensor() destructor called");
        if (initialized) {
            // Clean up resources
            cleanup();
        }
    }
    
    void init(int p) {
        pin = p;
        initialized = true;
        Serial.print("Sensor initialized on pin ");
        Serial.println(pin);
    }
    
    void cleanup() {
        Serial.print("Sensor on pin ");
        Serial.print(pin);
        Serial.println(" cleaned up");
    }
    
    int readValue() {
        return initialized ? analogRead(pin) : -1;
    }
};

// Example 2: Trivial POD structure (efficient, no overhead)
struct SensorReading {
    int value;
    unsigned long timestamp;
    // No constructor/destructor - will be zero-initialized efficiently
};

// Example usage in Arduino sketch
void setup() {
    Serial.begin(115200);
    
    // Initialize VMManager with filesystem
    if (!VMManager::instance().begin(SPIFFS, "/swap.dat")) {
        Serial.println("Failed to initialize VMManager");
        return;
    }
    
    Serial.println("\n=== Example 1: VMArray with Non-Trivial Types ===");
    {
        // VMArray automatically calls Sensor() constructor for each element
        VMArray<Sensor, 3> sensors;
        
        // Initialize each sensor
        sensors[0].init(A0);
        sensors[1].init(A1);
        sensors[2].init(A2);
        
        // Use sensors...
        for (size_t i = 0; i < 3; i++) {
            int value = sensors[i].readValue();
            Serial.print("Sensor ");
            Serial.print(i);
            Serial.print(" value: ");
            Serial.println(value);
        }
        
        // When sensors goes out of scope, ~Sensor() is automatically called
        // for each element, ensuring proper cleanup
        Serial.println("Leaving scope, destructors will be called...");
    }
    
    Serial.println("\n=== Example 2: VMArray with Trivial Types (Efficient) ===");
    {
        // VMArray with trivial types is zero-initialized efficiently
        // No constructor/destructor overhead
        VMArray<SensorReading, 10> readings;
        
        // All readings start with value=0, timestamp=0
        Serial.print("Initial reading[0]: value=");
        Serial.print(readings[0].value);
        Serial.print(", timestamp=");
        Serial.println(readings[0].timestamp);
        
        // Fill with data
        for (size_t i = 0; i < 10; i++) {
            readings[i].value = analogRead(A0);
            readings[i].timestamp = millis();
        }
    }
    
    Serial.println("\n=== Example 3: VMPtr with destroy() ===");
    {
        // Create a sensor using make_vm (calls constructor)
        auto sensorPtr = make_vm<Sensor>();
        sensorPtr->init(A3);
        
        // Use the sensor
        int value = sensorPtr->readValue();
        Serial.print("Dynamic sensor value: ");
        Serial.println(value);
        
        // Explicitly destroy when done (calls destructor and frees memory)
        Serial.println("Calling destroy()...");
        sensorPtr.destroy();
        
        // sensorPtr is now null and should not be used
        Serial.println("Sensor destroyed and memory freed");
    }
    
    Serial.println("\n=== Example 4: VMPtr with Trivial Type ===");
    {
        // VMPtr works efficiently with trivial types too
        auto dataPtr = make_vm<int>();
        *dataPtr = 42;
        
        Serial.print("Data value: ");
        Serial.println(*dataPtr);
        
        // destroy() works with trivial types (just frees memory, no destructor call)
        dataPtr.destroy();
        Serial.println("Data destroyed");
    }
    
    Serial.println("\n=== All Examples Complete ===");
}

void loop() {
    // Nothing to do
    delay(1000);
}

/**
 * Key Takeaways:
 * 
 * 1. VMArray<NonTrivialType, N>:
 *    - Automatically calls constructor for each element in constructor
 *    - Automatically calls destructor for each element in destructor
 *    - Behaves like std::array for object lifetime
 * 
 * 2. VMArray<TrivialType, N>:
 *    - Efficiently zero-initializes memory (no per-element overhead)
 *    - No constructor/destructor calls needed
 *    - Optimal performance for POD types
 * 
 * 3. VMPtr::destroy():
 *    - Explicitly ends object lifetime (calls destructor for non-trivial types)
 *    - Frees VM storage
 *    - Safe to call multiple times
 *    - Works efficiently with both trivial and non-trivial types
 * 
 * 4. The implementation uses C++17 type traits:
 *    - std::is_trivially_default_constructible<T>
 *    - std::is_trivially_destructible<T>
 *    - Zero runtime overhead for trivial types
 *    - Proper lifetime management for non-trivial types
 */
