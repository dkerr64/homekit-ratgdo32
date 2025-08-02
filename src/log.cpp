/****************************************************************************
 * RATGDO HomeKit for ESP32
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-25 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 * Contributions acknowledged from
 * Brandon Matthews... https://github.com/thenewwazoo
 * Jonathan Stroud...  https://github.com/jgstroud
 *
 */

// C/C++ language includes
#include <stdint.h>

// Arduino includes
#include <WiFiUdp.h>
#ifndef ESP8266
#include <esp32-hal.h>
#include <esp_core_dump.h>
#endif

// RATGDO project includes
#include "ratgdo.h"
#include "config.h"
#include "utilities.h"
#include "web.h"

// Logger tag
static const char *TAG = "ratgdo-logger";

// Construct the singleton object for logger access
LOG *LOG::instancePtr = new LOG();
LOG *ratgdoLogger = LOG::getInstance();

void logToSyslog(char *message);
bool syslogEn = false;
uint16_t syslogPort = 514;
char syslogIP[IP4ADDR_STRLEN_MAX] = "";
WiFiUDP syslog;
bool suppressSerialLog = false;
esp_log_level_t logLevel = ESP_LOG_VERBOSE;

#ifdef ESP8266
// These are defined in the linker script, and filled in by the elf2bin.py util
extern "C" uint32_t __crc_len;
extern "C" uint32_t __crc_val;
// Keep track of number of times crashed
int16_t crashCount;
// ESP8266 is single core / single threaded, no mutex's.
#define TAKE_MUTEX()
#define GIVE_MUTEX()

void crashCallback()
{
    if (ratgdoLogger->msgBuffer && ratgdoLogger->logMessageFile)
    {
        ratgdoLogger->logMessageFile.truncate(0);
        ratgdoLogger->logMessageFile.seek(0, fs::SeekSet);
        ratgdoLogger->logMessageFile.println();
        ratgdoLogger->logMessageFile.write(ESP.checkFlashCRC() ? "Flash CRC OK" : "Flash CRC BAD");
        ratgdoLogger->logMessageFile.println();
        ratgdoLogger->printMessageLog(ratgdoLogger->logMessageFile);
        ratgdoLogger->logMessageFile.close();
    }
    // We may not have enough memory to open the file and save the code
    // save_rolling_code();
}

void logToBuffer_P(const char *fmt, ...)
{
    if (ratgdoLogger)
    {
        va_list args;
        va_start(args, fmt);
        ratgdoLogger->logToBuffer(fmt, args);
        va_end(args);
    }
}
#else
// There is 8KB of RTC memory that can be set to not initialize on restart.
// Data saved here will survive a crash and restart, but will not survive a power interruption.
RTC_NOINIT_ATTR logBuffer rtcRebootLog;
RTC_NOINIT_ATTR logBuffer rtcCrashLog;
RTC_NOINIT_ATTR time_t rebootTime;
RTC_NOINIT_ATTR time_t crashTime;
RTC_NOINIT_ATTR int16_t crashCount;
RTC_NOINIT_ATTR char reasonString[64];
RTC_NOINIT_ATTR char crashVersion[16];
const int rtcSize = sizeof(rtcRebootLog) + sizeof(rtcCrashLog) + sizeof(rebootTime) + sizeof(crashTime) + sizeof(crashCount) + sizeof(reasonString) + sizeof(crashVersion);

#define TAKE_MUTEX() xSemaphoreTakeRecursive(logMutex, portMAX_DELAY)
#define GIVE_MUTEX() xSemaphoreGiveRecursive(logMutex)

void panic_handler(arduino_panic_info_t *info, void *arg)
{
    // crashCount could be negative... indicating that there is a core dump image, but no saved crash log.
    // But now we are saving a crash log, so need to make sure it is positive.
    crashCount = (crashCount < 0) ? 1 : crashCount + 1;
    crashTime = (clockSet) ? time(NULL) : 0;
    esp_rom_printf("Panic Handler, crash count %d\n", crashCount);
    memcpy(&rtcCrashLog, ratgdoLogger->msgBuffer, sizeof(rtcCrashLog));
    strlcpy(reasonString, info->reason, sizeof(reasonString));
    strlcpy(crashVersion, AUTO_VERSION, sizeof(crashVersion));
}
#endif

// Constructor for LOG class
LOG::LOG()
{
#ifdef ESP8266
    IRAM_START
    // IRAM heap is used only for allocating globals, to leave as much regular heap
    // available during operations.  We need to carefully monitor useage so as not
    // to exceed available IRAM.  We can adjust the LOG_BUFFER_SIZE (in log.h) if we
    // need to make more space available for initialization.
    msgBuffer = (logBuffer *)malloc(sizeof(logBuffer));
    lineBuffer = (char *)malloc(LINE_BUFFER_SIZE);
    // Open logMessageFile so we don't have to later.
    logMessageFile = (LittleFS.exists(CRASH_LOG_MSG_FILE)) ? LittleFS.open(CRASH_LOG_MSG_FILE, "r+") : LittleFS.open(CRASH_LOG_MSG_FILE, "w+");
    IRAM_END("Log buffers allocated");
#else
    logMutex = xSemaphoreCreateRecursiveMutex();
    msgBuffer = (logBuffer *)malloc(sizeof(logBuffer));
    lineBuffer = (char *)malloc(LINE_BUFFER_SIZE);
    set_arduino_panic_handler(panic_handler, NULL);
#endif
    // Zero out the buffer... because if we crash and dump buffer before it fills
    // up, we want blank space not garbage!
    memset(msgBuffer->buffer, 0, sizeof(msgBuffer->buffer));
    msgBuffer->wrapped = 0;
    msgBuffer->head = 0;
}

void LOG::logToBuffer(const char *fmt, va_list args)
{
    if (!lineBuffer)
    {
        static char buf[LINE_BUFFER_SIZE];
        // parse the format string into lineBuffer
        vsnprintf(buf, LINE_BUFFER_SIZE, fmt, args);
        //  print line to the serial port
        if (!suppressSerialLog)
            Serial.print(buf);
        return;
    }

    TAKE_MUTEX();
    // parse the format string into lineBuffer
    vsnprintf(lineBuffer, LINE_BUFFER_SIZE, fmt, args);
    // If timestamp is wrapped in () and not [] then message is from one of the ESP_LOGx() functions.
    // Insert a period into the milliseconds timestamp, so number of seconds is easier to read.
    if (strchr(lineBuffer, '(') - lineBuffer == 2)
    {
        char *closeBracket = strchr(lineBuffer, ')');
        int i = closeBracket - lineBuffer;
        if (i > 6)
        {
            memmove(&closeBracket[-2], &closeBracket[-3], LINE_BUFFER_SIZE - (i - 1));
            closeBracket[-3] = '.';
        }
    }

    //  print line to the serial port
    if (!suppressSerialLog)
        Serial.print(lineBuffer);

    // copy the line into the message save buffer
    size_t len = strlen(lineBuffer);
    size_t available = sizeof(msgBuffer->buffer) - msgBuffer->head;
    memcpy(&msgBuffer->buffer[msgBuffer->head], lineBuffer, min(available, len));
    if (available < len)
    {
        // we wrapped on the available buffer space
        msgBuffer->wrapped = 1;
        msgBuffer->head = len - available;
        memcpy(msgBuffer->buffer, &lineBuffer[available], msgBuffer->head);
    }
    else
    {
        msgBuffer->head += len;
    }
    msgBuffer->buffer[msgBuffer->head] = 0; // null terminate

    static bool inFn = false;
    if (!inFn)
    {
        // Control recursion... make sure we don't get into a loop if any
        // of the functions we use here log a message.  This is known to happen
        // in NetworkUDP code in error condition... used for SysLog.
        inFn = true;
        // send it to subscribed browsers
        SSEBroadcastState(lineBuffer, LOG_MESSAGE);
        // send it to syslog server
        logToSyslog(lineBuffer);
        inFn = false;
    }
    GIVE_MUTEX();
    return;
}

#ifndef ESP8266
// on ESP8266 we don't need this... crash callback saves log to file.
void LOG::printCrashLog(Print &outputDev)
{
    TAKE_MUTEX();
    if (crashCount > 0)
    {
        outputDev.printf("Time of crash: %s\n", timeString(crashTime));
        outputDev.printf("Crash reason: %s\n", reasonString);
        outputDev.printf("Firmware version: %s\n\n", crashVersion);
        // head points to a zero (null terminator of previous log line) which we need to skip.
        size_t start = (rtcCrashLog.head + 1) % sizeof(rtcCrashLog.buffer);
        if (rtcCrashLog.wrapped != 0)
        {
            outputDev.write(&rtcCrashLog.buffer[start], sizeof(rtcCrashLog.buffer) - start);
        }
        outputDev.print(rtcCrashLog.buffer); // assumes null terminated
    }

    if (esp_core_dump_image_check() == ESP_OK)
    {
        esp_core_dump_summary_t *summary = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
        if (summary)
        {
            if (esp_core_dump_get_summary(summary) == ESP_OK)
            {
                if (crashCount <= 0)
                    outputDev.print("No saved crash log available for core dump.");

                outputDev.print("\n\n");
                outputDev.printf("Crash in task: %s, at address: 0x%08lX\n", summary->exc_task, summary->exc_pc);
                outputDev.print("Decode backtrace with this Linux command:\n\n");
                if (crashCount > 0)
                    outputDev.printf("addr2line -p -f -C -e homekit-ratgdo32-v%s.elf \\\n", crashVersion);
                else
                    outputDev.print("addr2line -p -f -C -e firmware.elf \\\n");
                outputDev.print(" -a ");
                for (int i = 0; i < summary->exc_bt_info.depth; i++)
                {
                    outputDev.printf("0x%08lX ", summary->exc_bt_info.bt[i]);
                    if (((i + 1) % 6) == 0 && ((i + 1) < summary->exc_bt_info.depth))
                        outputDev.print("\\\n    ");
                }
                outputDev.print("\n\n");
                outputDev.print("Make sure that the ELF file matches the binary that crashed.\n");
            }
        }
        free(summary);
    }
    else
    {
        outputDev.print("\n\nNo core dump image available.\n");
    }
    GIVE_MUTEX();
}

void LOG::saveMessageLog(bool toNVram)
{
    ESP_LOGI(TAG, "Save message log buffer%s", toNVram ? "to NVRAM" : "");
    TAKE_MUTEX();
    // We start by rotating the circular buffer so it is all in order.
    uint16_t first = 0;
    uint16_t head = (msgBuffer->head + 1) % sizeof(msgBuffer->buffer); // adjust for null terminator.
    // uint16_t head = msgBuffer->head;
    uint16_t next = head;
    while (first != next)
    {
        std::swap(msgBuffer->buffer[first++], msgBuffer->buffer[next++]);
        if (next == sizeof(msgBuffer->buffer))
            next = head;
        else if (first == head)
            head = next;
    }
    //  reset the index
    msgBuffer->head = 0;
    msgBuffer->wrapped = 0;
    memcpy(&rtcRebootLog, ratgdoLogger->msgBuffer, sizeof(rtcRebootLog));
    rebootTime = (clockSet) ? time(NULL) : 0;
    if (toNVram)
        nvRam->writeBlob(nvram_messageLog, msgBuffer->buffer, sizeof(msgBuffer->buffer));
    GIVE_MUTEX();
}
#endif

#ifdef ESP8266
void LOG::printSavedLog(File file, Print &outputDev)
{
    if (file && file.size() > 0)
    {
        int num = LINE_BUFFER_SIZE;
        file.seek(0, fs::SeekSet);
        while (num == LINE_BUFFER_SIZE)
        {
            num = file.read((uint8_t *)lineBuffer, LINE_BUFFER_SIZE);
            outputDev.write(lineBuffer, num);
        }
        outputDev.println();
    }
}
#endif

void LOG::printSavedLog(Print &outputDev, bool fromNVram)
{
    ESP_LOGI(TAG, "Print saved log%s", fromNVram ? " from NVRAM" : "");
#ifdef ESP8266
    return printSavedLog(logMessageFile, outputDev);
#else
    if (fromNVram)
    {
        char *buf = (char *)malloc(sizeof(msgBuffer->buffer));
        if (buf)
        {
            nvRam->readBlob(nvram_messageLog, buf, sizeof(msgBuffer->buffer));
            outputDev.print(buf);
            free(buf);
        }
    }
    else if (rebootTime != 0)
    {
        outputDev.print(rtcRebootLog.buffer);
    }
    else
    {
        outputDev.print("\nNo saved log available\n");
    }
#endif
}

void LOG::printMessageLog(Print &outputDev)
{
    TAKE_MUTEX();
    if (enableNTP && clockSet)
    {
        outputDev.printf("Server time: %s\n", timeString());
    }
    outputDev.printf("Server uptime (secs): %u.%03u\n", (uint32_t)(_millis() / 1000LL), (uint16_t)(_millis() % 1000LL));
    outputDev.printf("Firmware version: %s\n", AUTO_VERSION);
#ifdef ESP8266
    outputDev.write("Flash CRC: 0x");
    outputDev.println(__crc_val, 16);
    outputDev.write("Flash length: ");
    outputDev.println(__crc_len);
#if defined(MMU_IRAM_HEAP)
    outputDev.write("IRAM heap size: ");
    outputDev.println(MMU_SEC_HEAP_SIZE);
#endif
#endif
    outputDev.printf("Free heap: %u\n", free_heap);
    outputDev.printf("Minimum heap: %u\n\n", min_heap);
    if (msgBuffer)
    {
        // head points to a zero (null terminator of previous log line) which we need to skip.
        size_t start = (msgBuffer->head + 1) % sizeof(msgBuffer->buffer);
        if (msgBuffer->wrapped != 0)
        {
            outputDev.write(&msgBuffer->buffer[start], sizeof(msgBuffer->buffer) - start);
        }
        outputDev.print(msgBuffer->buffer); // assumes null terminated
    }
    GIVE_MUTEX();
}

/****************************************************************************
 * Syslog
 */
#define SYSLOG_LOCAL0 16
#define SYSLOG_EMERGENCY 0
#define SYSLOG_ALERT 1
#define SYSLOG_CRIT 2
#define SYSLOG_ERROR 3
#define SYSLOG_WARN 4
#define SYSLOG_NOTICE 5
#define SYSLOG_INFO 6
#define SYSLOG_DEBUG 7
#define SYSLOG_NIL "-"
#define SYSLOG_BOM "\xEF\xBB\xBF"

void logToSyslog(char *message)
{
    if (!syslogEn || !WiFi.isConnected())
        return;

    uint8_t PRI = SYSLOG_LOCAL0 * 8;
    if (*message == '>')
        PRI += SYSLOG_INFO;
    else if (*message == '!')
        PRI += SYSLOG_ERROR;
    else if (*message == 'I')
        PRI += SYSLOG_INFO;
    else if (*message == 'E')
        PRI += SYSLOG_ERROR;
    else if (*message == 'W')
        PRI += SYSLOG_WARN;
    else if (*message == 'D')
        PRI += SYSLOG_DEBUG;
    else if (*message == 'V')
        PRI += SYSLOG_DEBUG;

    char *app_name;
    char *msg;
    static char unk[] = "unknown";

    // Strip out the TAG name which is embedded in the log message.
    // Can be bound by [timestamp] tagname: message
    // So extract from the close square bracket to the colon.
    // If no close square bracket, then message format may be
    // (timestamp) tagname:
    char *sqr = strchr(message, ']');
    char *brk = strchr(message, ')');
    if (sqr && brk)
    {
        if (sqr < brk)
            app_name = strtok(message, "]");
        else
            app_name = strtok(message, ")");
    }
    else if (sqr)
        app_name = strtok(message, "]");
    else if (brk)
        app_name = strtok(message, ")");
    else
        app_name = NULL;

    if (app_name)
    {
        while (*app_name == ' ')
            app_name++;
        app_name = strtok(NULL, ":");
        while (*app_name == ' ')
            app_name++;
        msg = strtok(NULL, "\r\n");
        while (*msg == ' ')
            msg++;
    }
    else
    {
        // neither a close close square or regular bracket then cannot determine tag name
        app_name = unk;
        msg = message;
    }
    syslog.beginPacket(syslogIP, syslogPort);
    // Use RFC5424 Format
    syslog.printf("<%u>1 ", PRI); // PRI code
#if defined(USE_NTP_TIMESTAMP)
    syslog.print((enableNTP && clockSet) ? timeString(0, true) : SYSLOG_NIL);
#else
    syslog.print(SYSLOG_NIL); // Time - let the syslog server insert time
#endif
    syslog.print(" ");
    syslog.print(device_name_rfc952); // hostname
    syslog.print(" ");
    syslog.print(app_name);     // application name
    syslog.printf(" 0");        // process ID
    syslog.print(" " SYSLOG_NIL // message ID
                 " " SYSLOG_NIL // structured data
#ifdef USE_UTF8_BOM
                 " " SYSLOG_BOM); // BOM - indicates UTF-8 encoding
#else
                 " "); // No BOM
#endif
    syslog.print(msg); // message
    syslog.endPacket();
}
