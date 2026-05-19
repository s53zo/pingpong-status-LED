#include "mqtt5_json_publisher.h"

#include <ESP8266WiFi.h>
#include <stdarg.h>

static constexpr uint32_t MQTT5_TIMEOUT_MS = 1000;

static void setError(char* error, size_t errorLen, const char* fmt, ...)
{
    if (!error || errorLen == 0) return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(error, errorLen, fmt, args);
    va_end(args);
    error[errorLen - 1] = '\0';
}

static size_t encodeVarInt(uint32_t value, uint8_t* out, size_t outLen)
{
    size_t count = 0;
    do {
        if (count >= outLen) return 0;
        uint8_t encoded = value % 128;
        value /= 128;
        if (value > 0) encoded |= 0x80;
        out[count++] = encoded;
    } while (value > 0);

    return count;
}

static size_t encodedVarIntLength(uint32_t value)
{
    size_t count = 0;
    do {
        value /= 128;
        ++count;
    } while (value > 0);

    return count;
}

static bool writeAll(WiFiClient& client, const uint8_t* data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        size_t n = client.write(data + written, len - written);
        if (n == 0) return false;
        written += n;
        yield();
    }
    return true;
}

static bool writeByte(WiFiClient& client, uint8_t value)
{
    return writeAll(client, &value, 1);
}

static bool writeUint16(WiFiClient& client, uint16_t value)
{
    uint8_t bytes[2] = {
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFF)
    };
    return writeAll(client, bytes, sizeof(bytes));
}

static bool writeVarInt(WiFiClient& client, uint32_t value)
{
    uint8_t bytes[4];
    size_t len = encodeVarInt(value, bytes, sizeof(bytes));
    return len > 0 && writeAll(client, bytes, len);
}

static bool writeUtf8String(WiFiClient& client, const char* value)
{
    if (!value) return false;
    size_t len = strlen(value);
    if (len > 65535) return false;

    return writeUint16(client, static_cast<uint16_t>(len)) &&
           writeAll(client, reinterpret_cast<const uint8_t*>(value), len);
}

static int readByteWithTimeout(WiFiClient& client, uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (!client.available()) {
        if (!client.connected() && !client.available()) return -1;
        if (millis() - start >= timeoutMs) return -1;
        delay(1);
    }
    return client.read();
}

static bool readVarInt(WiFiClient& client,
                       uint32_t timeoutMs,
                       uint32_t* value,
                       char* error,
                       size_t errorLen)
{
    uint32_t multiplier = 1;
    uint32_t result = 0;

    for (uint8_t i = 0; i < 4; ++i) {
        int encoded = readByteWithTimeout(client, timeoutMs);
        if (encoded < 0) {
            setError(error, errorLen, "timeout reading remaining length");
            return false;
        }

        result += (encoded & 127) * multiplier;
        if ((encoded & 128) == 0) {
            *value = result;
            return true;
        }
        multiplier *= 128;
    }

    setError(error, errorLen, "malformed remaining length");
    return false;
}

static bool writeFixedHeader(WiFiClient& client, uint8_t packetType, uint32_t remainingLength)
{
    uint8_t header[5];
    header[0] = packetType;
    size_t len = encodeVarInt(remainingLength, header + 1, sizeof(header) - 1);
    return len > 0 && writeAll(client, header, len + 1);
}

static bool sendConnect(WiFiClient& client, const char* clientId)
{
    const uint32_t remainingLength =
        2 + 4 +      // protocol name
        1 +          // protocol level
        1 +          // connect flags
        2 +          // keep alive
        1 +          // MQTT v5 properties length = 0
        2 + strlen(clientId);

    return writeFixedHeader(client, 0x10, remainingLength) &&
           writeUtf8String(client, "MQTT") &&
           writeByte(client, 5) &&
           writeByte(client, 0x02) &&
           writeUint16(client, 10) &&
           writeByte(client, 0) &&
           writeUtf8String(client, clientId);
}

static bool readConnack(WiFiClient& client, char* error, size_t errorLen)
{
    int packetType = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    if (packetType != 0x20) {
        setError(error, errorLen, "bad connack packet 0x%02X", packetType);
        return false;
    }

    uint32_t remainingLength = 0;
    if (!readVarInt(client, MQTT5_TIMEOUT_MS, &remainingLength, error, errorLen))
        return false;

    if (remainingLength < 2) {
        setError(error, errorLen, "short connack");
        return false;
    }

    int ackFlags = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    int reasonCode = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    if (ackFlags < 0 || reasonCode < 0) {
        setError(error, errorLen, "timeout reading connack");
        return false;
    }

    for (uint32_t i = 2; i < remainingLength; ++i) {
        if (readByteWithTimeout(client, MQTT5_TIMEOUT_MS) < 0) {
            setError(error, errorLen, "timeout draining connack");
            return false;
        }
    }

    if (reasonCode != 0) {
        setError(error, errorLen, "connack reason 0x%02X", reasonCode);
        return false;
    }

    return true;
}

static bool sendPublishJson(WiFiClient& client,
                            const char* topic,
                            const char* payload,
                            uint16_t packetId)
{
    const uint32_t topicLen = strlen(topic);
    const uint32_t payloadLen = strlen(payload);
    const uint32_t publishPropertiesLen =
        1 + 1 +      // payload format indicator = UTF-8 text
        1 + 2 + 4;   // content type id + UTF-8 "json"
    const uint32_t remainingLength =
        2 + topicLen +
        2 +
        encodedVarIntLength(publishPropertiesLen) +
        publishPropertiesLen +
        payloadLen;

    return writeFixedHeader(client, 0x32, remainingLength) &&
           writeUtf8String(client, topic) &&
           writeUint16(client, packetId) &&
           writeVarInt(client, publishPropertiesLen) &&
           writeByte(client, 0x01) &&
           writeByte(client, 0x01) &&
           writeByte(client, 0x03) &&
           writeUtf8String(client, "json") &&
           writeAll(client, reinterpret_cast<const uint8_t*>(payload), payloadLen);
}

static bool readPuback(WiFiClient& client,
                       uint16_t expectedPacketId,
                       char* error,
                       size_t errorLen)
{
    int packetType = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    if (packetType != 0x40) {
        setError(error, errorLen, "bad puback packet 0x%02X", packetType);
        return false;
    }

    uint32_t remainingLength = 0;
    if (!readVarInt(client, MQTT5_TIMEOUT_MS, &remainingLength, error, errorLen))
        return false;

    if (remainingLength < 2) {
        setError(error, errorLen, "short puback");
        return false;
    }

    int msb = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    int lsb = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
    if (msb < 0 || lsb < 0) {
        setError(error, errorLen, "timeout reading puback id");
        return false;
    }

    uint16_t packetId = (static_cast<uint16_t>(msb) << 8) |
                        static_cast<uint16_t>(lsb);
    if (packetId != expectedPacketId) {
        setError(error, errorLen, "puback id %u != %u",
                 packetId, expectedPacketId);
        return false;
    }

    uint8_t reasonCode = 0;
    if (remainingLength > 2) {
        int reason = readByteWithTimeout(client, MQTT5_TIMEOUT_MS);
        if (reason < 0) {
            setError(error, errorLen, "timeout reading puback reason");
            return false;
        }
        reasonCode = static_cast<uint8_t>(reason);
    }

    for (uint32_t i = (remainingLength > 2) ? 3 : 2; i < remainingLength; ++i) {
        if (readByteWithTimeout(client, MQTT5_TIMEOUT_MS) < 0) {
            setError(error, errorLen, "timeout draining puback");
            return false;
        }
    }

    if (reasonCode >= 0x80) {
        setError(error, errorLen, "puback reason 0x%02X", reasonCode);
        return false;
    }

    return true;
}

bool mqtt5PublishJson(const char* host,
                      uint16_t port,
                      const char* clientId,
                      const char* topic,
                      const char* payload,
                      char* error,
                      size_t errorLen)
{
    if (error && errorLen) error[0] = '\0';
    if (!host || !host[0] || !clientId || !clientId[0] || !topic || !topic[0] ||
        !payload) {
        setError(error, errorLen, "missing MQTT5 publish argument");
        return false;
    }

    WiFiClient client;
    client.setTimeout(MQTT5_TIMEOUT_MS);

    if (!client.connect(host, port)) {
        setError(error, errorLen, "connect %s:%u failed", host, port);
        return false;
    }

    bool ok = false;
    if (sendConnect(client, clientId)) {
        client.flush();
        const uint16_t packetId = 1;
        if (readConnack(client, error, errorLen) &&
            sendPublishJson(client, topic, payload, packetId)) {
            client.flush();
            ok = readPuback(client, packetId, error, errorLen);
        }
    }

    if (!ok && error && errorLen && error[0] == '\0')
        setError(error, errorLen, "MQTT5 publish failed");

    if (client.connected()) {
        const uint8_t disconnect[] = {0xE0, 0x00};
        writeAll(client, disconnect, sizeof(disconnect));
        client.flush();
    }
    client.stop();

    return ok;
}
