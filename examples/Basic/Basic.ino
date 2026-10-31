// MicroSwap basic example: a vector bigger than the RAM it uses.
//
// The swap file lives on an SD card (LittleFS on ESP8266 / RP2040).
// Wiring: SD card on the board's default SPI pins, chip select on SD_CS_PIN.

#include <MicroSwap.h>

#if defined(ESP8266) || defined(ARDUINO_ARCH_RP2040)
#include <LittleFS.h>
#else
#include <SPI.h>
#include <SD.h>
#endif

const int SD_CS_PIN = SS; // change to your SD card's chip select pin
const char* SWAP_PATH = "/swap.bin";

File swapFile;
VMFileStorage<File> swapStorage(swapFile);

/// Mount the file system and open the swap file for reading and writing.
bool openSwapFile() {
#if defined(ESP8266) || defined(ARDUINO_ARCH_RP2040)
    if (!LittleFS.begin()) return false;
    swapFile = LittleFS.open(SWAP_PATH, "w+");
#elif defined(ESP32)
    if (!SD.begin(SD_CS_PIN)) return false;
    swapFile = SD.open(SWAP_PATH, "w+");
#else
    if (!SD.begin(SD_CS_PIN)) return false;
    SD.remove(SWAP_PATH);
    // Not FILE_WRITE: it would append every write to the end of the file.
    swapFile = SD.open(SWAP_PATH, O_READ | O_WRITE | O_CREAT);
#endif
    return swapFile ? true : false;
}

void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
    while (true) delay(1000);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    if (!openSwapFile()) {
        Serial.println(F("Cannot open the swap file"));
        while (true) delay(1000);
    }
    VMManager& vm = VMManager::instance();
    vm.set_error_handler(onVMError);
    vm.begin(swapStorage);

    Serial.print(F("RAM used for pages: "));
    Serial.print(vm.get_page_size() * vm.get_resident_pages());
    Serial.print(F(" bytes, virtual memory: "));
    Serial.print(vm.get_page_size() * vm.get_page_count());
    Serial.println(F(" bytes"));

    // 4000 bytes of data, much more than an Uno's 2 KB of RAM.
    VMVector<long> squares;
    for (long i = 0; i < 1000; ++i) squares.push_back(i * i);

    long long sum = 0;
    for (size_t i = 0; i < squares.size(); ++i) sum += squares[i];
    Serial.print(F("Sum of squares 0..999: "));
    Serial.println((unsigned long)sum); // 332833500

    VMString greeting("Hello");
    greeting += ", MicroSwap!";
    Serial.println(greeting.c_str());

    Serial.print(F("Pages swapped in: "));
    Serial.print(vm.swap_in_count());
    Serial.print(F(", out: "));
    Serial.println(vm.swap_out_count());
}

void loop() {}
