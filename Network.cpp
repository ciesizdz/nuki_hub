#include "Network.h"
#include "PreferencesKeys.h"
#include "networkDevices/W5500Device.h"
#include "networkDevices/WifiDevice.h"
#include "Logger.h"
#include "Config.h"
#include <ArduinoJson.h>
#include "RestartReason.h"
#include "networkDevices/EthLan8720Device.h"

Network* Network::_inst = nullptr;
unsigned long Network::_ignoreSubscriptionsTs = 0;
bool _versionPublished = false;

RTC_NOINIT_ATTR char WiFi_fallbackDetect[14];

Network::Network(Preferences *preferences, Gpio* gpio, const String& maintenancePathPrefix, char* buffer, size_t bufferSize)
: _preferences(preferences),
  _gpio(gpio),
  _buffer(buffer),
  _bufferSize(bufferSize)
{
    // Remove obsolete W5500 hardware detection configuration
    if(_preferences->getInt(preference_network_hardware_gpio) != 0)
    {
        _preferences->remove(preference_network_hardware_gpio);
    }

    _inst = this;
    _hostname = _preferences->getString(preference_hostname);

    memset(_maintenancePathPrefix, 0, sizeof(_maintenancePathPrefix));
    size_t len = maintenancePathPrefix.length();
    for(int i=0; i < len; i++)
    {
        _maintenancePathPrefix[i] = maintenancePathPrefix.charAt(i);
    }

    _lockPath = _preferences->getString(preference_mqtt_lock_path);
    String connectionStateTopic = _lockPath + mqtt_topic_mqtt_connection_state;

    memset(_mqttConnectionStateTopic, 0, sizeof(_mqttConnectionStateTopic));
    len = connectionStateTopic.length();
    for(int i=0; i < len; i++)
    {
        _mqttConnectionStateTopic[i] = connectionStateTopic.charAt(i);
    }

    setupDevice();
}

void Network::setupDevice()
{
    _ipConfiguration = new IPConfiguration(_preferences);

    int hardwareDetect = _preferences->getInt(preference_network_hardware);

    Log->print(F("Hardware detect     : ")); Log->println(hardwareDetect);

    if(hardwareDetect == 0)
    {
        hardwareDetect = 1;
        _preferences->putInt(preference_network_hardware, hardwareDetect);
    }

    if(strcmp(WiFi_fallbackDetect, "wifi_fallback") == 0)
    {
        if(_preferences->getBool(preference_network_wifi_fallback_disabled))
        {
            Log->println(F("Failed to connect to network. Wi-Fi fallback is disabled, rebooting."));
            memset(WiFi_fallbackDetect, 0, sizeof(WiFi_fallbackDetect));
            sleep(5);
            restartEsp(RestartReason::NetworkDeviceCriticalFailureNoWifiFallback);
        }

        Log->println(F("Switching to Wi-Fi device as fallback."));
        _networkDeviceType = NetworkDeviceType::WiFi;
    }
    else
    {
        Log->print(F("Network device: "));
        switch (hardwareDetect)
        {
            case 1:
                Log->println(F("Wi-Fi only"));
                _networkDeviceType = NetworkDeviceType::WiFi;
                break;
            case 2:
                Log->print(F("Generic W5500"));
                _networkDeviceType = NetworkDeviceType::W5500;
                break;
            case 3:
                Log->println(F("W5500 on M5Stack Atom POE"));
                _networkDeviceType = NetworkDeviceType::W5500;
                break;
            case 4:
                Log->println(F("Olimex ESP32-POE / ESP-POE-ISO"));
                _networkDeviceType = NetworkDeviceType::Olimex_LAN8720;
                break;
            case 5:
                Log->println(F("WT32-ETH01"));
                _networkDeviceType = NetworkDeviceType::WT32_LAN8720;
                break;
            case 6:
                Log->println(F("M5STACK PoESP32 Unit"));
                _networkDeviceType = NetworkDeviceType::M5STACK_PoESP32_Unit;
                break;
            case 7:
                Log->println(F("LilyGO T-ETH-POE"));
                _networkDeviceType = NetworkDeviceType::LilyGO_T_ETH_POE;
                break;
            default:
                Log->println(F("Unknown hardware selected, falling back to Wi-Fi."));
                _networkDeviceType = NetworkDeviceType::WiFi;
                break;
        }
    }

    switch (_networkDeviceType)
    {
        case NetworkDeviceType::W5500:
            _device = new W5500Device(_hostname, _preferences, _ipConfiguration, hardwareDetect);
            break;
        case NetworkDeviceType::Olimex_LAN8720:
            _device = new EthLan8720Device(_hostname, _preferences, _ipConfiguration, "Olimex (LAN8720)", ETH_PHY_ADDR, 12, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_GPIO17_OUT);
            break;
        case NetworkDeviceType::WT32_LAN8720:
            _device = new EthLan8720Device(_hostname, _preferences, _ipConfiguration, "WT32-ETH01", 1, 16);
            break;
        case NetworkDeviceType::M5STACK_PoESP32_Unit:
            _device = new EthLan8720Device(_hostname, _preferences, _ipConfiguration, "M5STACK PoESP32 Unit", 1, 5, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_IP101);
            break;
        case NetworkDeviceType::LilyGO_T_ETH_POE:
            _device = new EthLan8720Device(_hostname, _preferences, _ipConfiguration, "LilyGO T-ETH-POE", 0, -1, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_GPIO17_OUT);
            break;
        case NetworkDeviceType::WiFi:
            _device = new WifiDevice(_hostname, _preferences, _ipConfiguration);
            break;
        default:
            _device = new WifiDevice(_hostname, _preferences, _ipConfiguration);
            break;
    }

    _device->mqttOnConnect([&](bool sessionPresent)
        {
            onMqttConnect(sessionPresent);
        });
    _device->mqttOnDisconnect([&](espMqttClientTypes::DisconnectReason reason)
        {
            onMqttDisconnect(reason);
        });
}

void Network::initialize()
{
    _restartOnDisconnect = _preferences->getBool(preference_restart_on_disconnect);
    _rssiPublishInterval = _preferences->getInt(preference_rssi_publish_interval) * 1000;

    _hostname = _preferences->getString(preference_hostname);

    if(_hostname == "")
    {
        _hostname = "nukihub";
        _preferences->putString(preference_hostname, _hostname);
    }
    if(_rssiPublishInterval == 0)
    {
        _rssiPublishInterval = 60;
        _preferences->putInt(preference_rssi_publish_interval, _rssiPublishInterval);
    }
    strcpy(_hostnameArr, _hostname.c_str());
    _device->initialize();

    Log->print(F("Host name: "));
    Log->println(_hostname);

    String brokerAddr = _preferences->getString(preference_mqtt_broker);
    strcpy(_mqttBrokerAddr, brokerAddr.c_str());

    int port = _preferences->getInt(preference_mqtt_broker_port);
    if(port == 0)
    {
        port = 1883;
        _preferences->putInt(preference_mqtt_broker_port, port);
    }

    String mqttUser = _preferences->getString(preference_mqtt_user);
    if(mqttUser.length() > 0)
    {
        size_t len = mqttUser.length();
        for(int i=0; i < len; i++)
        {
            _mqttUser[i] = mqttUser.charAt(i);
        }
    }

    String mqttPass = _preferences->getString(preference_mqtt_password);
    if(mqttPass.length() > 0)
    {
        size_t len = mqttPass.length();
        for(int i=0; i < len; i++)
        {
            _mqttPass[i] = mqttPass.charAt(i);
        }
    }

    Log->print(F("MQTT Broker: "));
    Log->print(_mqttBrokerAddr);
    Log->print(F(":"));
    Log->println(port);

    _device->mqttSetClientId(_hostnameArr);
    _device->mqttSetCleanSession(MQTT_CLEAN_SESSIONS);

    _networkTimeout = _preferences->getInt(preference_network_timeout);
    if(_networkTimeout == 0)
    {
        _networkTimeout = -1;
        _preferences->putInt(preference_network_timeout, _networkTimeout);
    }

    _publishDebugInfo = _preferences->getBool(preference_publish_debug_info);

    char gpioPath[250];
    bool rebGpio = rebuildGpio();

    if(rebGpio)
    {
        Log->println(F("Rebuild MQTT GPIO structure"));
    }
    for (const auto &pinEntry: _gpio->pinConfiguration())
    {
        switch (pinEntry.role)
        {
            case PinRole::GeneralInputPullDown:
            case PinRole::GeneralInputPullUp:
                if(rebGpio)
                {
                    buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pinEntry.pin)).c_str(), mqtt_topic_gpio_role});
                    publishString(_lockPath.c_str(), gpioPath, "input");
                    buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pinEntry.pin)).c_str(), mqtt_topic_gpio_state});
                    publishString(_lockPath.c_str(), gpioPath, std::to_string(digitalRead(pinEntry.pin)).c_str());
                }
                break;
            case PinRole::GeneralOutput:
                if(rebGpio)
                {
                    buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pinEntry.pin)).c_str(), mqtt_topic_gpio_role});
                    publishString(_lockPath.c_str(), gpioPath, "output");
                    buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pinEntry.pin)).c_str(), mqtt_topic_gpio_state});
                    publishString(_lockPath.c_str(), gpioPath, "0");
                }
                buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pinEntry.pin)).c_str(), mqtt_topic_gpio_state});
                subscribe(_lockPath.c_str(), gpioPath);
                break;
        }
    }
    _gpio->addCallback([this](const GpioAction& action, const int& pin)
    {
        gpioActionCallback(action, pin);
    });
}

bool Network::update()
{
    unsigned long ts = millis();

    _device->update();

    if(!_mqttEnabled)
    {
        return true;
    }

    if(!_device->isConnected())
    {
        if(_restartOnDisconnect && millis() > 60000)
        {
            restartEsp(RestartReason::RestartOnDisconnectWatchdog);
        }

        Log->println(F("Network not connected. Trying reconnect."));
        ReconnectStatus reconnectStatus = _device->reconnect();

        switch(reconnectStatus)
        {
            case ReconnectStatus::CriticalFailure:
                strcpy(WiFi_fallbackDetect, "wifi_fallback");
                Log->println("Network device has a critical failure, enable fallback to Wi-Fi and reboot.");
                delay(200);
                restartEsp(RestartReason::NetworkDeviceCriticalFailure);
                break;
            case ReconnectStatus::Success:
                memset(WiFi_fallbackDetect, 0, sizeof(WiFi_fallbackDetect));
                Log->println(F("Reconnect successful"));
                break;
            case ReconnectStatus::Failure:
                Log->println(F("Reconnect failed"));
                break;
        }

    }

    if(!_device->mqttConnected())
    {
        if(_networkTimeout > 0 && (ts - _lastConnectedTs > _networkTimeout * 1000) && ts > 60000)
        {
            Log->println("Network timeout has been reached, restarting ...");
            delay(200);
            restartEsp(RestartReason::NetworkTimeoutWatchdog);
        }

        bool success = reconnect();
        if(!success)
        {
            return false;
        }
    }

    _lastConnectedTs = ts;

    if(_presenceCsv != nullptr && strlen(_presenceCsv) > 0)
    {
        bool success = publishString(_mqttPresencePrefix, mqtt_topic_presence, _presenceCsv);
        if(!success)
        {
            Log->println(F("Failed to publish presence CSV data."));
            Log->println(_presenceCsv);
        }
        _presenceCsv = nullptr;
    }

    if(_device->signalStrength() != 127 && _rssiPublishInterval > 0 && ts - _lastRssiTs > _rssiPublishInterval)
    {
        _lastRssiTs = ts;
        int8_t rssi = _device->signalStrength();

        if(rssi != _lastRssi)
        {
            publishInt(_maintenancePathPrefix, mqtt_topic_wifi_rssi, _device->signalStrength());
            _lastRssi = rssi;
        }
    }

    if(_lastMaintenanceTs == 0 || (ts - _lastMaintenanceTs) > 30000)
    {
        publishULong(_maintenancePathPrefix, mqtt_topic_uptime, ts / 1000 / 60);
        if(_publishDebugInfo)
        {
            publishUInt(_maintenancePathPrefix, mqtt_topic_freeheap, esp_get_free_heap_size());
            publishString(_maintenancePathPrefix, mqtt_topic_restart_reason_fw, getRestartReason().c_str());
            publishString(_maintenancePathPrefix, mqtt_topic_restart_reason_esp, getEspRestartReason().c_str());
        }
        if (!_versionPublished) {
            publishString(_maintenancePathPrefix, mqtt_topic_info_nuki_hub_version, NUKI_HUB_VERSION);
            _versionPublished = true;
        }
        _lastMaintenanceTs = ts;
    }

    if(_preferences->getBool(preference_check_updates))
    {
        if(_lastUpdateCheckTs == 0 || (ts - _lastUpdateCheckTs) > 86400000)
        {
            _lastUpdateCheckTs = ts;

            https.useHTTP10(true);
            https.begin(GITHUB_LATEST_RELEASE_API_URL);

            int httpResponseCode = https.GET();

            if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_MOVED_PERMANENTLY)
            {
                DynamicJsonDocument doc(6144);
                DeserializationError jsonError = deserializeJson(doc, https.getStream());

                if (!jsonError)
                {
                    _latestVersion = doc["tag_name"];
                    publishString(_maintenancePathPrefix, mqtt_topic_info_nuki_hub_latest, _latestVersion);

                    if (_latestVersion != _preferences->getString(preference_latest_version).c_str())
                    {
                        _preferences->putString(preference_latest_version, _latestVersion);
                    }
                }
            }

            https.end();
        }
    }

    for(const auto& gpioTs : _gpioTs)
    {
        uint8_t pin = gpioTs.first;
        unsigned long ts = gpioTs.second;
        if(ts != 0 && ((millis() - ts) >= GPIO_DEBOUNCE_TIME))
        {
            _gpioTs[pin] = 0;

            uint8_t pinState = digitalRead(pin) == HIGH ? 1 : 0;
            char gpioPath[250];
            buildMqttPath(gpioPath, {mqtt_topic_gpio_prefix, (mqtt_topic_gpio_pin + std::to_string(pin)).c_str(), mqtt_topic_gpio_state});
            publishInt(_lockPath.c_str(), gpioPath, pinState);

            Log->print(F("GPIO "));
            Log->print(pin);
            Log->print(F(" (Input) --> "));
            Log->println(pinState);
        }
    }

    return true;
}


void Network::onMqttConnect(const bool &sessionPresent)
{
    _connectReplyReceived = true;
}

void Network::onMqttDisconnect(const espMqttClientTypes::DisconnectReason &reason)
{
    _connectReplyReceived = true;

    Log->print("MQTT disconnected. Reason: ");
    switch(reason)
    {
        case espMqttClientTypes::DisconnectReason::USER_OK:
            Log->println(F("USER_OK"));
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
            Log->println(F("MQTT_UNACCEPTABLE_PROTOCOL_VERSION"));
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_IDENTIFIER_REJECTED:
            Log->println(F("MQTT_IDENTIFIER_REJECTED"));
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_SERVER_UNAVAILABLE:
            Log->println(F("MQTT_SERVER_UNAVAILABLE"));
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_MALFORMED_CREDENTIALS:
            Log->println(F("MQTT_MALFORMED_CREDENTIALS"));
            break;
        case espMqttClientTypes::DisconnectReason::MQTT_NOT_AUTHORIZED:
            Log->println(F("MQTT_NOT_AUTHORIZED"));
            break;
        case espMqttClientTypes::DisconnectReason::TLS_BAD_FINGERPRINT:
            Log->println(F("TLS_BAD_FINGERPRINT"));
            break;
        case espMqttClientTypes::DisconnectReason::TCP_DISCONNECTED:
            Log->println(F("TCP_DISCONNECTED"));
            break;
        default:
            Log->println(F("Unknown"));
            break;
    }
}

bool Network::reconnect()
{
    _mqttConnectionState = 0;
    int port = _preferences->getInt(preference_mqtt_broker_port);

    while (!_device->mqttConnected() && millis() > _nextReconnect)
    {
        if(strcmp(_mqttBrokerAddr, "") == 0)
        {
            Log->println(F("MQTT Broker not configured, aborting connection attempt."));
            _nextReconnect = millis() + 5000;
            return false;
        }

        Log->println(F("Attempting MQTT connection"));

        _connectReplyReceived = false;

        if(strlen(_mqttUser) == 0)
        {
            Log->println(F("MQTT: Connecting without credentials"));
        }
        else
        {
            Log->print(F("MQTT: Connecting with user: ")); Log->println(_mqttUser);
            _device->mqttSetCredentials(_mqttUser, _mqttPass);
        }

        _device->setWill(_mqttConnectionStateTopic, 1, true, _lastWillPayload);
        _device->mqttSetServer(_mqttBrokerAddr, port);
        _device->mqttConnect();

        unsigned long timeout = millis() + 60000;

        while(!_connectReplyReceived && millis() < timeout)
        {
            delay(50);
            _device->update();
            if(_keepAliveCallback != nullptr)
            {
                _keepAliveCallback();
            }
        }

        if (_device->mqttConnected())
        {
            Log->println(F("MQTT connected"));
            _mqttConnectionState = 1;
            delay(100);

            _ignoreSubscriptionsTs = millis() + 2000;
            _device->mqttOnMessage(Network::onMqttDataReceivedCallback);
            for(const String& topic : _subscribedTopics)
            {
                _device->mqttSubscribe(topic.c_str(), MQTT_QOS_LEVEL);
            }
            if(_firstConnect)
            {
                _firstConnect = false;
                publishString(_maintenancePathPrefix, mqtt_topic_network_device, _device->deviceName().c_str());
                for(const auto& it : _initTopics)
                {
                    _device->mqttPublish(it.first.c_str(), MQTT_QOS_LEVEL, true, it.second.c_str());
                }
            }

            publishString(_maintenancePathPrefix, mqtt_topic_mqtt_connection_state, "online");
            publishString(_maintenancePathPrefix, mqtt_topic_info_nuki_hub_ip, _device->localIP().c_str());

            _mqttConnectionState = 2;
            for(const auto& callback : _reconnectedCallbacks)
            {
                callback();
            }
        }
        else
        {
            Log->print(F("MQTT connect failed, rc="));
            _device->printError();
            _mqttConnectionState = 0;
            _nextReconnect = millis() + 5000;
            _device->mqttDisconnect(true);
        }
    }
    return _mqttConnectionState > 0;
}

void Network::subscribe(const char* prefix, const char *path)
{
    char prefixedPath[500];
    buildMqttPath(prefixedPath, { prefix, path });
    _subscribedTopics.push_back(prefixedPath);
}

void Network::initTopic(const char *prefix, const char *path, const char *value)
{
    char prefixedPath[500];
    buildMqttPath(prefixedPath, { prefix, path });
    String pathStr = prefixedPath;
    String valueStr = value;
    _initTopics[pathStr] = valueStr;
}

void Network::buildMqttPath(char* outPath, std::initializer_list<const char*> paths)
{
    int offset = 0;
    int pathCount = 0;

    for(const char* path : paths)
    {
        if(pathCount > 0 && path[0] != '/')
        {
            outPath[offset] = '/';
            ++offset;
        }

        int i = 0;
        while(path[i] != 0)
        {
            outPath[offset] = path[i];
            ++offset;
            ++i;
        }
        ++pathCount;
    }

    outPath[offset] = 0x00;
}

void Network::registerMqttReceiver(MqttReceiver* receiver)
{
    _mqttReceivers.push_back(receiver);
}

void Network::onMqttDataReceivedCallback(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len, size_t index, size_t total)
{
    uint8_t value[50] = {0};
    size_t l = min(len, sizeof(value)-1);

    for(int i=0; i<l; i++)
    {
        value[i] = payload[i];
    }

    _inst->onMqttDataReceived(properties, topic, value, len, index, total);
}

void Network::onMqttDataReceived(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t& len, size_t& index, size_t& total)
{
    parseGpioTopics(properties, topic, payload, len, index, total);

    if(millis() < _ignoreSubscriptionsTs)
    {
        return;
    }

    for(auto receiver : _mqttReceivers)
    {
        receiver->onMqttDataReceived(topic, (byte*)payload, index);
    }
}


void Network::parseGpioTopics(const espMqttClientTypes::MessageProperties &properties, const char *topic, const uint8_t *payload, size_t& len, size_t& index, size_t& total)
{
    char gpioPath[250];
    buildMqttPath(gpioPath, {_lockPath.c_str(), mqtt_topic_gpio_prefix, mqtt_topic_gpio_pin});
//    /nuki_t/gpio/pin_17/state
    size_t gpioLen = strlen(gpioPath);
    if(strncmp(gpioPath, topic, gpioLen) == 0)
    {
        char pinStr[3] = {0};
        pinStr[0] = topic[gpioLen];
        if(topic[gpioLen+1] != '/')
        {
            pinStr[1] = topic[gpioLen+1];
        }

        int pin = std::atoi(pinStr);

        if(_gpio->getPinRole(pin) == PinRole::GeneralOutput)
        {
            const uint8_t pinState = strcmp((const char*)payload, "1") == 0 ? HIGH : LOW;
            Log->print(F("GPIO "));
            Log->print(pin);
            Log->print(F(" (Output) --> "));
            Log->println(pinState);
            digitalWrite(pin, pinState);
        }

    }
}

void Network::gpioActionCallback(const GpioAction &action, const int &pin)
{
    _gpioTs[pin] = millis();
}

void Network::reconfigureDevice()
{
    _device->reconfigure();
}

void Network::setMqttPresencePath(char *path)
{
    memset(_mqttPresencePrefix, 0, sizeof(_mqttPresencePrefix));
    strcpy(_mqttPresencePrefix, path);
}

void Network::disableAutoRestarts()
{
    _networkTimeout = 0;
    _restartOnDisconnect = false;
}

int Network::mqttConnectionState()
{
    return _mqttConnectionState;
}

bool Network::encryptionSupported()
{
    return _device->supportsEncryption();
}

const String Network::networkDeviceName() const
{
    return _device->deviceName();
}

void Network::publishFloat(const char* prefix, const char* topic, const float value, const uint8_t precision)
{
    char str[30];
    dtostrf(value, 0, precision, str);
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    _device->mqttPublish(path, MQTT_QOS_LEVEL, true, str);
}

void Network::publishInt(const char* prefix, const char *topic, const int value)
{
    char str[30];
    itoa(value, str, 10);
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    _device->mqttPublish(path, MQTT_QOS_LEVEL, true, str);
}

void Network::publishUInt(const char* prefix, const char *topic, const unsigned int value)
{
    char str[30];
    utoa(value, str, 10);
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    _device->mqttPublish(path, MQTT_QOS_LEVEL, true, str);
}

void Network::publishULong(const char* prefix, const char *topic, const unsigned long value)
{
    char str[30];
    utoa(value, str, 10);
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    _device->mqttPublish(path, MQTT_QOS_LEVEL, true, str);
}

void Network::publishBool(const char* prefix, const char *topic, const bool value)
{
    char str[2] = {0};
    str[0] = value ? '1' : '0';
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    _device->mqttPublish(path, MQTT_QOS_LEVEL, true, str);
}

bool Network::publishString(const char* prefix, const char *topic, const char *value)
{
    char path[200] = {0};
    buildMqttPath(path, { prefix, topic });
    return _device->mqttPublish(path, MQTT_QOS_LEVEL, true, value) > 0;
}

void Network::publishHASSConfig(char* deviceType, const char* baseTopic, char* name, char* uidString, const char* availabilityTopic, const bool& hasKeypad, char* lockAction, char* unlockAction, char* openAction)
{
    DynamicJsonDocument json(JSON_BUFFER_SIZE);

    auto dev = json.createNestedObject("dev");
    auto ids = dev.createNestedArray("ids");
    ids.add(String("nuki_") + uidString);
    json["dev"]["mf"] = "Nuki";
    json["dev"]["mdl"] = deviceType;
    json["dev"]["name"] = name;

    String cuUrl = _preferences->getString(preference_mqtt_hass_cu_url);

    if (cuUrl != "")
    {
        json["dev"]["cu"] = cuUrl;
    }
    else
    {
        json["dev"]["cu"] = "http://" + _device->localIP();
    }

    json["~"] = baseTopic;
    json["name"] = nullptr;
    json["unique_id"] = String(uidString) + "_lock";
    json["cmd_t"] = String("~") + String(mqtt_topic_lock_action);
    json["avty"]["t"] = availabilityTopic;
    json["pl_lock"] = lockAction;
    json["pl_unlk"] = unlockAction;
    json["pl_open"] = openAction;
    json["stat_t"] = String("~") + mqtt_topic_lock_ha_state;
    json["stat_jammed"] = "jammed";
    json["stat_locked"] = "locked";
    json["stat_locking"] = "locking";
    json["stat_unlocked"] = "unlocked";
    json["stat_unlocking"] = "unlocking";
    json["opt"] = "false";

    serializeJson(json, _buffer, _bufferSize);

    String path = _preferences->getString(preference_mqtt_hass_discovery);
    path.concat("/lock/");
    path.concat(uidString);
    path.concat("/smartlock/config");

    _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);

    // Battery critical
    publishHassTopic("binary_sensor",
                     "battery_low",
                     uidString,
                     "_battery_low",
                     "Battery low",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_battery_critical,
                     deviceType,
                     "battery",
                     "",
                     "diagnostic",
                     "",
                     {{"pl_on", "1"},
                      {"pl_off", "0"}});

    // Battery voltage
    publishHassTopic("sensor",
                     "battery_voltage",
                     uidString,
                     "_battery_voltage",
                     "Battery voltage",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_battery_voltage,
                     deviceType,
                     "voltage",
                     "measurement",
                     "diagnostic",
                     "",
                     { {"unit_of_meas", "V"} });

    // Trigger
    publishHassTopic("sensor",
                     "trigger",
                     uidString,
                     "_trigger",
                     "Trigger",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_trigger,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "en", "true" } });

    // MQTT Connected
    publishHassTopic("binary_sensor",
                     "mqtt_connected",
                     uidString,
                     "_mqtt_connected",
                     "MQTT connected",
                     name,
                     baseTopic,
                     _lockPath + mqtt_topic_mqtt_connection_state,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     {{"pl_on", "online"},
                      {"pl_off", "offline"},
                      {"ic", "mdi:lan-connect"}});

    // Reset
    publishHassTopic("switch",
                     "reset",
                     uidString,
                     "_reset",
                     "Restart Nuki Hub",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_reset,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_reset,
                     { { "ic", "mdi:restart" },
                       { "pl_on", "1" },
                       { "pl_off", "0" },
                       { "stat_on", "1" },
                       { "stat_off", "0" }});

    // Firmware version
    publishHassTopic("sensor",
                     "firmware_version",
                     uidString,
                     "_firmware_version",
                     "Firmware version",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_info_firmware_version,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "en", "true" },
                       {"ic", "mdi:counter"}});

    // Hardware version
    publishHassTopic("sensor",
                     "hardware_version",
                     uidString,
                     "_hardware_version",
                     "Hardware version",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_info_hardware_version,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "en", "true" },
                       {"ic", "mdi:counter"}});

    // Nuki Hub version
    publishHassTopic("sensor",
                     "nuki_hub_version",
                     uidString,
                     "_nuki_hub_version",
                     "Nuki Hub version",
                     name,
                     baseTopic,
                     _lockPath + mqtt_topic_info_nuki_hub_version,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "en", "true" },
                       {"ic", "mdi:counter"}});

    if(_preferences->getBool(preference_check_updates))
    {
        // NUKI Hub latest
        publishHassTopic("sensor",
                         "nuki_hub_latest",
                         uidString,
                         "_nuki_hub_latest",
                         "NUKI Hub latest",
                         name,
                         baseTopic,
                         _lockPath + mqtt_topic_info_nuki_hub_latest,
                         deviceType,
                         "",
                         "",
                         "diagnostic",
                         "",
                         { { "en", "true" },
                           {"ic", "mdi:counter"}});

        // NUKI Hub update
        char latest_version_topic[250];
        _lockPath.toCharArray(latest_version_topic,_lockPath.length() + 1);
        strcat(latest_version_topic, mqtt_topic_info_nuki_hub_latest);

        publishHassTopic("update",
                         "nuki_hub_update",
                         uidString,
                         "_nuki_hub_update",
                         "NUKI Hub firmware update",
                         name,
                         baseTopic,
                         _lockPath + mqtt_topic_info_nuki_hub_version,
                         deviceType,
                         "firmware",
                         "",
                         "diagnostic",
                         "",
                         { { "en", "true" },
                           { "ent_pic", "https://raw.githubusercontent.com/technyon/nuki_hub/master/icon/favicon-32x32.png" },
                           { "rel_u", GITHUB_LATEST_RELEASE_URL },
                           { "l_ver_t", latest_version_topic }});
    }
    else
    {
        removeHassTopic("sensor", "nuki_hub_latest", uidString);
        removeHassTopic("update", "nuki_hub_update", uidString);
    }

    // Nuki Hub IP Address
    publishHassTopic("sensor",
                     "nuki_hub_ip",
                     uidString,
                     "_nuki_hub_ip",
                     "Nuki Hub IP",
                     name,
                     baseTopic,
                     _lockPath + mqtt_topic_info_nuki_hub_ip,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "en", "true" },
                       {"ic", "mdi:ip"}});

    // Query Lock State
    publishHassTopic("button",
                     "query_lockstate",
                     uidString,
                     "_query_lockstate_button",
                     "Query lock state",
                     name,
                     baseTopic,
                     "",
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_query_lockstate,
                     { { "en", "false" },
                       { "pl_prs", "1" }});

    // Query Config
    publishHassTopic("button",
                     "query_config",
                     uidString,
                     "_query_config_button",
                     "Query config",
                     name,
                     baseTopic,
                     "",
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_query_config,
                     { { "en", "false" },
                       { "pl_prs", "1" }});

    // Query Lock State Command result
    publishHassTopic("button",
                     "query_commandresult",
                     uidString,
                     "_query_commandresult_button",
                     "Query lock state command result",
                     name,
                     baseTopic,
                     "",
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_query_lockstate_command_result,
                     { { "en", "false" },
                       { "pl_prs", "1" }});

    publishHassTopic("sensor",
                     "bluetooth_signal_strength",
                     uidString,
                     "_bluetooth_signal_strength",
                     "Bluetooth signal strength",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_rssi,
                     deviceType,
                     "signal_strength",
                     "measurement",
                     "diagnostic",
                     "",
                     { {"unit_of_meas", "dBm"} });
}


void Network::publishHASSConfigAdditionalLockEntities(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    uint32_t aclPrefs[17];
    _preferences->getBytes(preference_acl, &aclPrefs, sizeof(aclPrefs));
    uint32_t basicLockConfigAclPrefs[16];
    _preferences->getBytes(preference_conf_lock_basic_acl, &basicLockConfigAclPrefs, sizeof(basicLockConfigAclPrefs));
    uint32_t advancedLockConfigAclPrefs[22];
    _preferences->getBytes(preference_conf_lock_advanced_acl, &advancedLockConfigAclPrefs, sizeof(advancedLockConfigAclPrefs));

    if((int)aclPrefs[2])
    {
        // Unlatch
        publishHassTopic("button",
                         "unlatch",
                         uidString,
                         "_unlatch_button",
                         "Open",
                         name,
                         baseTopic,
                         "",
                         deviceType,
                         "",
                         "",
                         "",
                         String("~") + mqtt_topic_lock_action,
                         { { "en", "false" },
                           { "pl_prs", "unlatch" }});
    }
    else
    {
        removeHassTopic("button", "unlatch", uidString);
    }

    if((int)aclPrefs[3])
    {
        // Lock 'n' Go
        publishHassTopic("button",
                         "lockngo",
                         uidString,
                         "_lock_n_go_button",
                         "Lock 'n' Go",
                         name,
                         baseTopic,
                         "",
                         deviceType,
                         "",
                         "",
                         "",
                         String("~") + mqtt_topic_lock_action,
                         { { "en", "false" },
                           { "pl_prs", "lockNgo" }});
    }
    else
    {
        removeHassTopic("button", "lockngo", uidString);
    }

    if((int)aclPrefs[4])
    {
        // Lock 'n' Go with unlatch
        publishHassTopic("button",
                         "lockngounlatch",
                         uidString,
                         "_lock_n_go_unlatch_button",
                         "Lock 'n' Go with unlatch",
                         name,
                         baseTopic,
                         "",
                         deviceType,
                         "",
                         "",
                         "",
                         String("~") + mqtt_topic_lock_action,
                         { { "en", "false" },
                           { "pl_prs", "lockNgoUnlatch" }});
    }
    else
    {
        removeHassTopic("button", "lockngounlatch", uidString);
    }

    // Query Battery
    publishHassTopic("button",
                     "query_battery",
                     uidString,
                     "_query_battery_button",
                     "Query battery",
                     name,
                     baseTopic,
                     "",
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_query_battery,
                     { { "en", "false" },
                       { "pl_prs", "1" }});

    if((int)basicLockConfigAclPrefs[6] == 1)
    {
        // LED enabled
        publishHassTopic("switch",
                         "led_enabled",
                         uidString,
                         "_led_enabled",
                         "LED enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "ic", "mdi:led-variant-on" },
                           { "pl_on", "{ \"ledEnabled\": \"1\"}" },
                           { "pl_off", "{ \"ledEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.ledEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "led_enabled", uidString);
    }

    if((int)basicLockConfigAclPrefs[5] == 1)
    {
        // Button enabled
        publishHassTopic("switch",
                         "button_enabled",
                         uidString,
                         "_button_enabled",
                         "Button enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "ic", "mdi:radiobox-marked" },
                           { "pl_on", "{ \"buttonEnabled\": \"1\"}" },
                           { "pl_off", "{ \"buttonEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.buttonEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "button_enabled", uidString);
    }

    if((int)advancedLockConfigAclPrefs[19] == 1)
    {
        // Auto Lock
        publishHassTopic("switch",
                         "auto_lock",
                         uidString,
                         "_auto_lock",
                         "Auto lock",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"autoLockEnabled\": \"1\"}" },
                           { "pl_off", "{ \"autoLockEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.autoLockEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "auto_lock", uidString);
    }

    if((int)advancedLockConfigAclPrefs[12] == 1)
    {
        // Auto Unlock
        publishHassTopic("switch",
                         "auto_unlock",
                         uidString,
                         "_auto_unlock",
                         "Auto unlock",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"autoUnLockDisabled\": \"0\"}" },
                           { "pl_off", "{ \"autoUnLockDisabled\": \"1\"}" },
                           { "val_tpl", "{{value_json.autoUnLockDisabled}}" },
                           { "stat_on", "0" },
                           { "stat_off", "1" }});
    }
    else
    {
        removeHassTopic("switch", "auto_unlock", uidString);
    }

    if((int)basicLockConfigAclPrefs[13] == 1)
    {
        // Double lock
        publishHassTopic("switch",
                         "double_lock",
                         uidString,
                         "_double_lock",
                         "Double lock",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"singleLock\": \"0\"}" },
                           { "pl_off", "{ \"singleLock\": \"1\"}" },
                           { "val_tpl", "{{value_json.singleLock}}" },
                           { "stat_on", "0" },
                           { "stat_off", "1" }});
    }
    else
    {
        removeHassTopic("switch", "double_lock", uidString);
    }

    publishHassTopic("sensor",
                     "battery_level",
                     uidString,
                     "_battery_level",
                     "Battery level",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_battery_level,
                     deviceType,
                     "battery",
                     "measurement",
                     "diagnostic",
                     "",
                     { {"unit_of_meas", "%"} });

    if((int)basicLockConfigAclPrefs[7] == 1)
    {
        publishHassTopic("number",
                         "led_brightness",
                         uidString,
                         "_led_brightness",
                         "LED brightness",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "ic", "mdi:brightness-6" },
                           { "cmd_tpl", "{ \"ledBrightness\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.ledBrightness}}" },
                           { "min", "0" },
                           { "max", "5" }});
    }
    else
    {
        removeHassTopic("number", "led_brightness", uidString);
    }

    if((int)basicLockConfigAclPrefs[3] == 1)
    {
        // Auto Unlatch
        publishHassTopic("switch",
                         "auto_unlatch",
                         uidString,
                         "_auto_unlatch",
                         "Auto unlatch",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"autoUnlatch\": \"1\"}" },
                           { "pl_off", "{ \"autoUnlatch\": \"0\"}" },
                           { "val_tpl", "{{value_json.autoUnlatch}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "auto_unlatch", uidString);
    }

    if((int)basicLockConfigAclPrefs[4] == 1)
    {
        // Pairing enabled
        publishHassTopic("switch",
                         "pairing_enabled",
                         uidString,
                         "_pairing_enabled",
                         "Pairing enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"pairingEnabled\": \"1\"}" },
                           { "pl_off", "{ \"pairingEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.pairingEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "pairing_enabled", uidString);
    }

    if((int)basicLockConfigAclPrefs[8] == 1)
    {
        publishHassTopic("number",
                         "timezone_offset",
                         uidString,
                         "_timezone_offset",
                         "Timezone offset",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "ic", "mdi:timer-cog-outline" },
                           { "cmd_tpl", "{ \"timeZoneOffset\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.timeZoneOffset}}" },
                           { "min", "0" },
                           { "max", "60" }});
    }
    else
    {
        removeHassTopic("number", "timezone_offset", uidString);
    }

    if((int)basicLockConfigAclPrefs[9] == 1)
    {
        // DST Mode
        publishHassTopic("switch",
                         "dst_mode",
                         uidString,
                         "_dst_mode",
                         "DST mode European",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"dstMode\": \"1\"}" },
                           { "pl_off", "{ \"dstMode\": \"0\"}" },
                           { "val_tpl", "{{value_json.dstMode}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "dst_mode", uidString);
    }

    if((int)basicLockConfigAclPrefs[10] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_1", "Fob action 1", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction1}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction1\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Unlock";
        json["options"][2] = "Lock";
        json["options"][3] = "Lock n Go";
        json["options"][4] = "Intelligent";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_1", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_1", uidString);
    }

    if((int)basicLockConfigAclPrefs[11] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_2", "Fob action 2", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction2}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction2\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Unlock";
        json["options"][2] = "Lock";
        json["options"][3] = "Lock n Go";
        json["options"][4] = "Intelligent";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_2", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_2", uidString);
    }

    if((int)basicLockConfigAclPrefs[12] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_3", "Fob action 3", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction3}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction3\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Unlock";
        json["options"][2] = "Lock";
        json["options"][3] = "Lock n Go";
        json["options"][4] = "Intelligent";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_3", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_3", uidString);
    }

    if((int)basicLockConfigAclPrefs[14] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_advertising_mode", "Advertising mode", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.advertisingMode}}" }, { "en", "false" }, { "cmd_tpl", "{ \"advertisingMode\": \"{{ value }}\" }" }});
        json["options"][0] = "Automatic";
        json["options"][1] = "Normal";
        json["options"][2] = "Slow";
        json["options"][3] = "Slowest";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "advertising_mode", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "advertising_mode", uidString);
    }

    if((int)basicLockConfigAclPrefs[15] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_timezone", "Timezone", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.timeZone}}" }, { "en", "false" }, { "cmd_tpl", "{ \"timeZone\": \"{{ value }}\" }" }});
        json["options"][0] = "Africa/Cairo";
        json["options"][1] = "Africa/Lagos";
        json["options"][2] = "Africa/Maputo";
        json["options"][3] = "Africa/Nairobi";
        json["options"][4] = "America/Anchorage";
        json["options"][5] = "America/Argentina/Buenos_Aires";
        json["options"][6] = "America/Chicago";
        json["options"][7] = "America/Denver";
        json["options"][8] = "America/Halifax";
        json["options"][9] = "America/Los_Angeles";
        json["options"][10] = "America/Manaus";
        json["options"][11] = "America/Mexico_City";
        json["options"][12] = "America/New_York";
        json["options"][13] = "America/Phoenix";
        json["options"][14] = "America/Regina";
        json["options"][15] = "America/Santiago";
        json["options"][16] = "America/Sao_Paulo";
        json["options"][17] = "America/St_Johns";
        json["options"][18] = "Asia/Bangkok";
        json["options"][19] = "Asia/Dubai";
        json["options"][20] = "Asia/Hong_Kong";
        json["options"][21] = "Asia/Jerusalem";
        json["options"][22] = "Asia/Karachi";
        json["options"][23] = "Asia/Kathmandu";
        json["options"][24] = "Asia/Kolkata";
        json["options"][25] = "Asia/Riyadh";
        json["options"][26] = "Asia/Seoul";
        json["options"][27] = "Asia/Shanghai";
        json["options"][28] = "Asia/Tehran";
        json["options"][29] = "Asia/Tokyo";
        json["options"][30] = "Asia/Yangon";
        json["options"][31] = "Australia/Adelaide";
        json["options"][32] = "Australia/Brisbane";
        json["options"][33] = "Australia/Darwin";
        json["options"][34] = "Australia/Hobart";
        json["options"][35] = "Australia/Perth";
        json["options"][36] = "Australia/Sydney";
        json["options"][37] = "Europe/Berlin";
        json["options"][38] = "Europe/Helsinki";
        json["options"][39] = "Europe/Istanbul";
        json["options"][40] = "Europe/London";
        json["options"][41] = "Europe/Moscow";
        json["options"][42] = "Pacific/Auckland";
        json["options"][43] = "Pacific/Guam";
        json["options"][44] = "Pacific/Honolulu";
        json["options"][45] = "Pacific/Pago_Pago";
        json["options"][46] = "None";

        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "timezone", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "timezone", uidString);
    }

    if((int)advancedLockConfigAclPrefs[0] == 1)
    {
        publishHassTopic("number",
                         "unlocked_position_offset_degrees",
                         uidString,
                         "_unlocked_position_offset_degrees",
                         "Unlocked position offset degrees",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"unlockedPositionOffsetDegrees\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.unlockedPositionOffsetDegrees}}" },
                           { "min", "-90" },
                           { "max", "180" }});
    }
    else
    {
        removeHassTopic("number", "unlocked_position_offset_degrees", uidString);
    }

    if((int)advancedLockConfigAclPrefs[1] == 1)
    {
        publishHassTopic("number",
                         "locked_position_offset_degrees",
                         uidString,
                         "_locked_position_offset_degrees",
                         "Locked position offset degrees",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"lockedPositionOffsetDegrees\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.lockedPositionOffsetDegrees}}" },
                           { "min", "-180" },
                           { "max", "90" }});
    }
    else
    {
        removeHassTopic("number", "locked_position_offset_degrees", uidString);
    }

    if((int)advancedLockConfigAclPrefs[2] == 1)
    {
        publishHassTopic("number",
                         "single_locked_position_offset_degrees",
                         uidString,
                         "_single_locked_position_offset_degrees",
                         "Single locked position offset degrees",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"singleLockedPositionOffsetDegrees\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.singleLockedPositionOffsetDegrees}}" },
                           { "min", "-180" },
                           { "max", "180" }});
    }
    else
    {
        removeHassTopic("number", "single_locked_position_offset_degrees", uidString);
    }

    if((int)advancedLockConfigAclPrefs[3] == 1)
    {
        publishHassTopic("number",
                         "unlocked_locked_transition_offset_degrees",
                         uidString,
                         "_unlocked_locked_transition_offset_degrees",
                         "Unlocked to locked transition offset degrees",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"unlockedToLockedTransitionOffsetDegrees\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.unlockedToLockedTransitionOffsetDegrees}}" },
                           { "min", "-180" },
                           { "max", "180" }});
    }
    else
    {
        removeHassTopic("number", "unlocked_locked_transition_offset_degrees", uidString);
    }

    if((int)advancedLockConfigAclPrefs[4] == 1)
    {
        publishHassTopic("number",
                         "lockngo_timeout",
                         uidString,
                         "_lockngo_timeout",
                         "Lock n Go timeout",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"lockNgoTimeout\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.lockNgoTimeout}}" },
                           { "min", "5" },
                           { "max", "60" }});
    }
    else
    {
        removeHassTopic("number", "lockngo_timeout", uidString);
    }

    if((int)advancedLockConfigAclPrefs[5] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_single_button_press_action", "Single button press action", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.singleButtonPressAction}}" }, { "en", "false" }, { "cmd_tpl", "{ \"singleButtonPressAction\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Intelligent";
        json["options"][2] = "Unlock";
        json["options"][3] = "Lock";
        json["options"][4] = "Unlatch";
        json["options"][5] = "Lock n Go";
        json["options"][6] = "Show Status";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "single_button_press_action", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "single_button_press_action", uidString);
    }

    if((int)advancedLockConfigAclPrefs[6] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_double_button_press_action", "Double button press action", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.doubleButtonPressAction}}" }, { "en", "false" }, { "cmd_tpl", "{ \"doubleButtonPressAction\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Intelligent";
        json["options"][2] = "Unlock";
        json["options"][3] = "Lock";
        json["options"][4] = "Unlatch";
        json["options"][5] = "Lock n Go";
        json["options"][6] = "Show Status";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "double_button_press_action", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "double_button_press_action", uidString);
    }

    if((int)advancedLockConfigAclPrefs[7] == 1)
    {
        // Detached cylinder
        publishHassTopic("switch",
                         "detached_cylinder",
                         uidString,
                         "_detached_cylinder",
                         "Detached cylinder",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"detachedCylinder\": \"1\"}" },
                           { "pl_off", "{ \"detachedCylinder\": \"0\"}" },
                           { "val_tpl", "{{value_json.detachedCylinder}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "detached_cylinder", uidString);
    }

    if((int)advancedLockConfigAclPrefs[8] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_battery_type", "Battery type", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.batteryType}}" }, { "en", "false" }, { "cmd_tpl", "{ \"batteryType\": \"{{ value }}\" }" }});
        json["options"][0] = "Alkali";
        json["options"][1] = "Accumulators";
        json["options"][2] = "Lithium";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "battery_type", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "battery_type", uidString);
    }

    if((int)advancedLockConfigAclPrefs[9] == 1)
    {
        // Automatic battery type detection
        publishHassTopic("switch",
                         "automatic_battery_type_detection",
                         uidString,
                         "_automatic_battery_type_detection",
                         "Automatic battery type detection",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"automaticBatteryTypeDetection\": \"1\"}" },
                           { "pl_off", "{ \"automaticBatteryTypeDetection\": \"0\"}" },
                           { "val_tpl", "{{value_json.automaticBatteryTypeDetection}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "automatic_battery_type_detection", uidString);
    }

    if((int)advancedLockConfigAclPrefs[10] == 1)
    {
        publishHassTopic("number",
                         "unlatch_duration",
                         uidString,
                         "_unlatch_duration",
                         "Unlatch duration",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"unlatchDuration\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.unlatchDuration}}" },
                           { "min", "1" },
                           { "max", "30" }});
    }
    else
    {
        removeHassTopic("number", "unlatch_duration", uidString);
    }

    if((int)advancedLockConfigAclPrefs[11] == 1)
    {
        publishHassTopic("number",
                         "auto_lock_timeout",
                         uidString,
                         "_auto_lock_timeout",
                         "Auto lock timeout",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"autoLockTimeOut\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.autoLockTimeOut}}" },
                           { "min", "30" },
                           { "max", "180" }});
    }
    else
    {
        removeHassTopic("number", "auto_lock_timeout", uidString);
    }

    if((int)advancedLockConfigAclPrefs[13] == 1)
    {
        // Nightmode enabled
        publishHassTopic("switch",
                         "nightmode_enabled",
                         uidString,
                         "_nightmode_enabled",
                         "Nightmode enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"nightModeEnabled\": \"1\"}" },
                           { "pl_off", "{ \"nightModeEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.nightModeEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "nightmode_enabled", uidString);
    }

    if((int)advancedLockConfigAclPrefs[14] == 1)
    {
        // Nightmode start time
        publishHassTopic("text",
                         "nightmode_start_time",
                         uidString,
                         "_nightmode_start_time",
                         "Nightmode start time",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pattern", "([0-1][0-9]|2[0-3]):[0-5][0-9]" },
                           { "cmd_tpl", "{ \"nightModeStartTime\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.nightModeStartTime}}" },
                           { "min", "5" },
                           { "max", "5" }});
    }
    else
    {
        removeHassTopic("text", "nightmode_start_time", uidString);
    }

    if((int)advancedLockConfigAclPrefs[15] == 1)
    {
        // Nightmode end time
        publishHassTopic("text",
                         "nightmode_end_time",
                         uidString,
                         "_nightmode_end_time",
                         "Nightmode end time",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pattern", "([0-1][0-9]|2[0-3]):[0-5][0-9]" },
                           { "cmd_tpl", "{ \"nightModeEndTime\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.nightModeEndTime}}" },
                           { "min", "5" },
                           { "max", "5" }});
    }
    else
    {
        removeHassTopic("text", "nightmode_end_time", uidString);
    }

    if((int)advancedLockConfigAclPrefs[16] == 1)
    {
        // Nightmode Auto Lock
        publishHassTopic("switch",
                         "nightmode_auto_lock",
                         uidString,
                         "_nightmode_auto_lock",
                         "Nightmode auto lock",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"nightModeAutoLockEnabled\": \"1\"}" },
                           { "pl_off", "{ \"nightModeAutoLockEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.nightModeAutoLockEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "nightmode_auto_lock", uidString);
    }

    if((int)advancedLockConfigAclPrefs[17] == 1)
    {
        // Nightmode Auto Unlock
        publishHassTopic("switch",
                         "nightmode_auto_unlock",
                         uidString,
                         "_nightmode_auto_unlock",
                         "Nightmode auto unlock",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"nightModeAutoUnlockDisabled\": \"0\"}" },
                           { "pl_off", "{ \"nightModeAutoUnlockDisabled\": \"1\"}" },
                           { "val_tpl", "{{value_json.nightModeAutoUnlockDisabled}}" },
                           { "stat_on", "0" },
                           { "stat_off", "1" }});
    }
    else
    {
        removeHassTopic("switch", "nightmode_auto_unlock", uidString);
    }

    if((int)advancedLockConfigAclPrefs[18] == 1)
    {
        // Nightmode immediate lock on start
        publishHassTopic("switch",
                         "nightmode_immediate_lock_start",
                         uidString,
                         "_nightmode_immediate_lock_start",
                         "Nightmode immediate lock on start",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"nightModeImmediateLockOnStart\": \"1\"}" },
                           { "pl_off", "{ \"nightModeImmediateLockOnStart\": \"0\"}" },
                           { "val_tpl", "{{value_json.nightModeImmediateLockOnStart}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "nightmode_immediate_lock_start", uidString);
    }

    if((int)advancedLockConfigAclPrefs[20] == 1)
    {
        // Immediate auto lock enabled
        publishHassTopic("switch",
                         "immediate_auto_lock_enabled",
                         uidString,
                         "_immediate_auto_lock_enabled",
                         "Immediate auto lock enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"immediateAutoLockEnabled\": \"1\"}" },
                           { "pl_off", "{ \"immediateAutoLockEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.immediateAutoLockEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "immediate_auto_lock_enabled", uidString);
    }

    if((int)advancedLockConfigAclPrefs[21] == 1)
    {
        // Auto update enabled
        publishHassTopic("switch",
                         "auto_update_enabled",
                         uidString,
                         "_auto_update_enabled",
                         "Auto update enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"autoUpdateEnabled\": \"1\"}" },
                           { "pl_off", "{ \"autoUpdateEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.autoUpdateEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "auto_update_enabled", uidString);
    }
}

void Network::publishHASSConfigDoorSensor(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    publishHassTopic("binary_sensor",
                     "door_sensor",
                     uidString,
                     "_door_sensor",
                     "Door sensor",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_door_sensor_state,
                     deviceType,
                     "door",
                     "",
                     "",
                     "",
                     {{"pl_on", "doorOpened"},
                      {"pl_off", "doorClosed"},
                      {"pl_not_avail", "unavailable"}});
}

void Network::publishHASSConfigAdditionalOpenerEntities(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    uint32_t aclPrefs[17];
    _preferences->getBytes(preference_acl, &aclPrefs, sizeof(aclPrefs));
    uint32_t basicOpenerConfigAclPrefs[16];
    _preferences->getBytes(preference_conf_opener_basic_acl, &basicOpenerConfigAclPrefs, sizeof(basicOpenerConfigAclPrefs));
    uint32_t advancedOpenerConfigAclPrefs[22];
    _preferences->getBytes(preference_conf_opener_advanced_acl, &advancedOpenerConfigAclPrefs, sizeof(advancedOpenerConfigAclPrefs));

    if((int)aclPrefs[11])
    {
        // Unlatch
        publishHassTopic("button",
                         "unlatch",
                         uidString,
                         "_unlatch_button",
                         "Open",
                         name,
                         baseTopic,
                         "",
                         deviceType,
                         "",
                         "",
                         "",
                         String("~") + mqtt_topic_lock_action,
                         { { "en", "false" },
                           { "pl_prs", "electricStrikeActuation" }});
    }
    else
    {
        removeHassTopic("button", "unlatch", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[5] == 1)
    {
        // LED enabled
        publishHassTopic("switch",
                         "led_enabled",
                         uidString,
                         "_led_enabled",
                         "LED enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "ic", "mdi:led-variant-on" },
                           { "pl_on", "{ \"ledEnabled\": \"1\"}" },
                           { "pl_off", "{ \"ledEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.ledEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "led_enabled", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[4] == 1)
    {
        // Button enabled
        publishHassTopic("switch",
                         "button_enabled",
                         uidString,
                         "_button_enabled",
                         "Button enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "ic", "mdi:radiobox-marked" },
                           { "pl_on", "{ \"buttonEnabled\": \"1\"}" },
                           { "pl_off", "{ \"buttonEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.buttonEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "button_enabled", uidString);
    }

    publishHassTopic("binary_sensor",
                     "continuous_mode",
                     uidString,
                     "_continuous_mode",
                     "Continuous mode",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_continuous_mode,
                     deviceType,
                     "lock",
                     "",
                     "",
                     "",
                     {{"pl_on", "on"},
                      {"pl_off", "off"}});

    if((int)aclPrefs[12] == 1 && (int)aclPrefs[13] == 1)
    {
        publishHassTopic("switch",
                         "continuous_mode",
                         uidString,
                         "_continuous_mode",
                         "Continuous mode",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_lock_continuous_mode,
                         deviceType,
                         "",
                         "",
                         "",
                         String("~") + mqtt_topic_lock_action,
                         {{ "en", "false" },
                          {"stat_on", "on"},
                          {"stat_off", "off"},
                          {"pl_on", "activateCM"},
                          {"pl_off", "deactivateCM"}});
    }
    else
    {
        removeHassTopic("switch", "continuous_mode", uidString);
    }

    publishHassTopic("binary_sensor",
                     "ring",
                     uidString,
                     "_ring_detect",
                     "Ring detect",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_binary_ring,
                     deviceType,
                     "sound",
                     "",
                     "",
                     "",
                     {{"pl_on", "ring"},
                      {"pl_off", "standby"}});

    DynamicJsonDocument json(_bufferSize);
    json = createHassJson(uidString, "_ring_event", "Ring", name, baseTopic, String("~") + mqtt_topic_lock_ring, deviceType, "doorbell", "", "", "", {{"val_tpl", "{ \"event_type\": \"{{ value }}\" }"}});
    json["event_types"][0] = "ring";
    json["event_types"][1] = "ringlocked";
    serializeJson(json, _buffer, _bufferSize);
    String path = createHassTopicPath("event", "ring", uidString);
    _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);

    if((int)advancedOpenerConfigAclPrefs[15] == 1)
    {
        publishHassTopic("number",
                         "sound_level",
                         uidString,
                         "_sound_level",
                         "Sound level",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "ic", "mdi:volume-source" },
                           { "cmd_tpl", "{ \"soundLevel\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.soundLevel}}" },
                           { "min", "0" },
                           { "max", "255" },
                           { "mode", "slider" },
                           { "step", "25.5" }});
    }
    else
    {
        removeHassTopic("number", "sound_level", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[3] == 1)
    {
        // Pairing enabled
        publishHassTopic("switch",
                         "pairing_enabled",
                         uidString,
                         "_pairing_enabled",
                         "Pairing enabled",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"pairingEnabled\": \"1\"}" },
                           { "pl_off", "{ \"pairingEnabled\": \"0\"}" },
                           { "val_tpl", "{{value_json.pairingEnabled}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "pairing_enabled", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[6] == 1)
    {
        publishHassTopic("number",
                         "timezone_offset",
                         uidString,
                         "_timezone_offset",
                         "Timezone offset",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "ic", "mdi:timer-cog-outline" },
                           { "cmd_tpl", "{ \"timeZoneOffset\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.timeZoneOffset}}" },
                           { "min", "0" },
                           { "max", "60" }});
    }
    else
    {
        removeHassTopic("number", "timezone_offset", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[7] == 1)
    {
        // DST Mode
        publishHassTopic("switch",
                         "dst_mode",
                         uidString,
                         "_dst_mode",
                         "DST mode European",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_basic_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"dstMode\": \"1\"}" },
                           { "pl_off", "{ \"dstMode\": \"0\"}" },
                           { "val_tpl", "{{value_json.dstMode}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "dst_mode", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[8] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_1", "Fob action 1", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction1}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction1\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Toggle RTO";
        json["options"][2] = "Activate RTO";
        json["options"][3] = "Deactivate RTO";
        json["options"][4] = "Open";
        json["options"][5] = "Ring";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_1", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_1", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[9] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_2", "Fob action 2", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction2}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction2\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Toggle RTO";
        json["options"][2] = "Activate RTO";
        json["options"][3] = "Deactivate RTO";
        json["options"][4] = "Open";
        json["options"][5] = "Ring";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_2", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_2", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[10] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_fob_action_3", "Fob action 3", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.fobAction3}}" }, { "en", "false" }, { "cmd_tpl", "{ \"fobAction3\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Toggle RTO";
        json["options"][2] = "Activate RTO";
        json["options"][3] = "Deactivate RTO";
        json["options"][4] = "Open";
        json["options"][5] = "Ring";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "fob_action_3", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "fob_action_3", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[12] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_advertising_mode", "Advertising mode", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.advertisingMode}}" }, { "en", "false" }, { "cmd_tpl", "{ \"advertisingMode\": \"{{ value }}\" }" }});
        json["options"][0] = "Automatic";
        json["options"][1] = "Normal";
        json["options"][2] = "Slow";
        json["options"][3] = "Slowest";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "advertising_mode", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "advertising_mode", uidString);
    }

    if((int)basicOpenerConfigAclPrefs[13] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_timezone", "Timezone", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.timeZone}}" }, { "en", "false" }, { "cmd_tpl", "{ \"timeZone\": \"{{ value }}\" }" }});
        json["options"][0] = "Africa/Cairo";
        json["options"][1] = "Africa/Lagos";
        json["options"][2] = "Africa/Maputo";
        json["options"][3] = "Africa/Nairobi";
        json["options"][4] = "America/Anchorage";
        json["options"][5] = "America/Argentina/Buenos_Aires";
        json["options"][6] = "America/Chicago";
        json["options"][7] = "America/Denver";
        json["options"][8] = "America/Halifax";
        json["options"][9] = "America/Los_Angeles";
        json["options"][10] = "America/Manaus";
        json["options"][11] = "America/Mexico_City";
        json["options"][12] = "America/New_York";
        json["options"][13] = "America/Phoenix";
        json["options"][14] = "America/Regina";
        json["options"][15] = "America/Santiago";
        json["options"][16] = "America/Sao_Paulo";
        json["options"][17] = "America/St_Johns";
        json["options"][18] = "Asia/Bangkok";
        json["options"][19] = "Asia/Dubai";
        json["options"][20] = "Asia/Hong_Kong";
        json["options"][21] = "Asia/Jerusalem";
        json["options"][22] = "Asia/Karachi";
        json["options"][23] = "Asia/Kathmandu";
        json["options"][24] = "Asia/Kolkata";
        json["options"][25] = "Asia/Riyadh";
        json["options"][26] = "Asia/Seoul";
        json["options"][27] = "Asia/Shanghai";
        json["options"][28] = "Asia/Tehran";
        json["options"][29] = "Asia/Tokyo";
        json["options"][30] = "Asia/Yangon";
        json["options"][31] = "Australia/Adelaide";
        json["options"][32] = "Australia/Brisbane";
        json["options"][33] = "Australia/Darwin";
        json["options"][34] = "Australia/Hobart";
        json["options"][35] = "Australia/Perth";
        json["options"][36] = "Australia/Sydney";
        json["options"][37] = "Europe/Berlin";
        json["options"][38] = "Europe/Helsinki";
        json["options"][39] = "Europe/Istanbul";
        json["options"][40] = "Europe/London";
        json["options"][41] = "Europe/Moscow";
        json["options"][42] = "Pacific/Auckland";
        json["options"][43] = "Pacific/Guam";
        json["options"][44] = "Pacific/Honolulu";
        json["options"][45] = "Pacific/Pago_Pago";
        json["options"][46] = "None";

        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "timezone", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "timezone", uidString);
    }
    
    if((int)basicOpenerConfigAclPrefs[11] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_operating_mode", "Operating mode", name, baseTopic, String("~") + mqtt_topic_config_basic_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.operatingMode}}" }, { "en", "false" }, { "cmd_tpl", "{ \"operatingMode\": \"{{ value }}\" }" }});
        json["options"][0] = "Generic door opener";
        json["options"][1] = "Analogue intercom";
        json["options"][2] = "Digital intercom";
        json["options"][3] = "Siedle";
        json["options"][4] = "TCS";
        json["options"][5] = "Bticino";
        json["options"][6] = "Siedle HTS";
        json["options"][7] = "STR";
        json["options"][8] = "Ritto";
        json["options"][9] = "Fermax";
        json["options"][10] = "Comelit";
        json["options"][11] = "Urmet BiBus";
        json["options"][12] = "Urmet 2Voice";
        json["options"][13] = "Golmar";
        json["options"][14] = "SKS";
        json["options"][15] = "Spare";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "operating_mode", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "operating_mode", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[1] == 1)
    {
        // BUS mode switch analogue
        publishHassTopic("switch",
                         "bus_mode_switch",
                         uidString,
                         "_bus_mode_switch",
                         "BUS mode switch analogue",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"busModeSwitch\": \"1\"}" },
                           { "pl_off", "{ \"busModeSwitch\": \"0\"}" },
                           { "val_tpl", "{{value_json.busModeSwitch}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "bus_mode_switch", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[2] == 1)
    {
        publishHassTopic("number",
                         "short_circuit_duration",
                         uidString,
                         "_short_circuit_duration",
                         "Short circuit duration",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"shortCircuitDuration\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.shortCircuitDuration}}" },
                           { "min", "0" }});
    }
    else
    {
        removeHassTopic("number", "short_circuit_duration", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[3] == 1)
    {
        publishHassTopic("number",
                         "electric_strike_delay",
                         uidString,
                         "_electric_strike_delay",
                         "Electric strike delay",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"electricStrikeDelay\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.electricStrikeDelay}}" },
                           { "min", "0" },
                           { "min", "30000" },
                           { "step", "3000" }});
    }
    else
    {
        removeHassTopic("number", "electric_strike_delay", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[4] == 1)
    {
        // Random Electric Strike Delay
        publishHassTopic("switch",
                         "random_electric_strike_delay",
                         uidString,
                         "_random_electric_strike_delay",
                         "Random electric strike delay",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"randomElectricStrikeDelay\": \"1\"}" },
                           { "pl_off", "{ \"randomElectricStrikeDelay\": \"0\"}" },
                           { "val_tpl", "{{value_json.randomElectricStrikeDelay}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "random_electric_strike_delay", uidString);
    }
    
    if((int)advancedOpenerConfigAclPrefs[5] == 1)
    {
        publishHassTopic("number",
                         "electric_strike_duration",
                         uidString,
                         "_electric_strike_duration",
                         "Electric strike duration",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"electricStrikeDuration\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.electricStrikeDuration}}" },
                           { "min", "1000" },
                           { "min", "30000" },
                           { "step", "3000" }});
    }
    else
    {
        removeHassTopic("number", "electric_strike_duration", uidString);
    }
    
    if((int)advancedOpenerConfigAclPrefs[6] == 1)
    {
        // Disable RTO after ring
        publishHassTopic("switch",
                         "disable_rto_after_ring",
                         uidString,
                         "_disable_rto_after_ring",
                         "Disable RTO after ring",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"disableRtoAfterRing\": \"1\"}" },
                           { "pl_off", "{ \"disableRtoAfterRing\": \"0\"}" },
                           { "val_tpl", "{{value_json.disableRtoAfterRing}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "disable_rto_after_ring", uidString);
    }
    
    if((int)advancedOpenerConfigAclPrefs[7] == 1)
    {
        publishHassTopic("number",
                         "rto_timeout",
                         uidString,
                         "_rto_timeout",
                         "RTO timeout",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"rtoTimeout\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.rtoTimeout}}" },
                           { "min", "5" },
                           { "min", "60" }});
    }
    else
    {
        removeHassTopic("number", "rto_timeout", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[8] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_doorbell_suppression", "Doorbell suppression", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.doorbellSuppression}}" }, { "en", "false" }, { "cmd_tpl", "{ \"doorbellSuppression\": \"{{ value }}\" }" }});
        json["options"][0] = "Off";
        json["options"][1] = "CM";
        json["options"][2] = "RTO";
        json["options"][3] = "CM & RTO";
        json["options"][4] = "Ring";
        json["options"][5] = "CM & Ring";
        json["options"][6] = "RTO & Ring";
        json["options"][7] = "CM & RTO & Ring";        
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "doorbell_suppression", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "doorbell_suppression", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[9] == 1)
    {
        publishHassTopic("number",
                         "doorbell_suppression_duration",
                         uidString,
                         "_doorbell_suppression_duration",
                         "Doorbell suppression duration",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "cmd_tpl", "{ \"doorbellSuppressionDuration\": \"{{ value }}\" }" },
                           { "val_tpl", "{{value_json.doorbellSuppressionDuration}}" },
                           { "min", "500" },
                           { "min", "10000" },
                           { "step", "1000" }});
    }
    else
    {
        removeHassTopic("number", "doorbell_suppression_duration", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[10] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_sound_ring", "Sound ring", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.soundRing}}" }, { "en", "false" }, { "cmd_tpl", "{ \"soundRing\": \"{{ value }}\" }" }});
        json["options"][0] = "No Sound";
        json["options"][1] = "Sound 1";
        json["options"][2] = "Sound 2";
        json["options"][3] = "Sound 3";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "sound_ring", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "sound_ring", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[11] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_sound_open", "Sound open", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.soundOpen}}" }, { "en", "false" }, { "cmd_tpl", "{ \"soundOpen\": \"{{ value }}\" }" }});
        json["options"][0] = "No Sound";
        json["options"][1] = "Sound 1";
        json["options"][2] = "Sound 2";
        json["options"][3] = "Sound 3";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "sound_open", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "sound_open", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[12] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_sound_rto", "Sound RTO", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.soundRto}}" }, { "en", "false" }, { "cmd_tpl", "{ \"soundRto\": \"{{ value }}\" }" }});
        json["options"][0] = "No Sound";
        json["options"][1] = "Sound 1";
        json["options"][2] = "Sound 2";
        json["options"][3] = "Sound 3";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "sound_rto", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "sound_rto", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[13] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_sound_cm", "Sound CM", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.soundCm}}" }, { "en", "false" }, { "cmd_tpl", "{ \"soundCm\": \"{{ value }}\" }" }});
        json["options"][0] = "No Sound";
        json["options"][1] = "Sound 1";
        json["options"][2] = "Sound 2";
        json["options"][3] = "Sound 3";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "sound_cm", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "sound_cm", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[14] == 1)
    {
        // Sound confirmation
        publishHassTopic("switch",
                         "sound_confirmation",
                         uidString,
                         "_sound_confirmation",
                         "Sound confirmation",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"soundConfirmation\": \"1\"}" },
                           { "pl_off", "{ \"soundConfirmation\": \"0\"}" },
                           { "val_tpl", "{{value_json.soundConfirmation}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "sound_confirmation", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[16] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_single_button_press_action", "Single button press action", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.singleButtonPressAction}}" }, { "en", "false" }, { "cmd_tpl", "{ \"singleButtonPressAction\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Toggle RTO";
        json["options"][2] = "Activate RTO";
        json["options"][3] = "Deactivate RTO";
        json["options"][4] = "Toggle CM";
        json["options"][5] = "Activate CM";
        json["options"][6] = "Deactivate CM";
        json["options"][7] = "Open";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "single_button_press_action", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "single_button_press_action", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[17] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_double_button_press_action", "Double button press action", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.doubleButtonPressAction}}" }, { "en", "false" }, { "cmd_tpl", "{ \"doubleButtonPressAction\": \"{{ value }}\" }" }});
        json["options"][0] = "No Action";
        json["options"][1] = "Toggle RTO";
        json["options"][2] = "Activate RTO";
        json["options"][3] = "Deactivate RTO";
        json["options"][4] = "Toggle CM";
        json["options"][5] = "Activate CM";
        json["options"][6] = "Deactivate CM";
        json["options"][7] = "Open";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "double_button_press_action", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "double_button_press_action", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[18] == 1)
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, "_battery_type", "Battery type", name, baseTopic, String("~") + mqtt_topic_config_advanced_json, deviceType, "", "", "config", String("~") + mqtt_topic_config_action, {{ "val_tpl", "{{value_json.batteryType}}" }, { "en", "false" }, { "cmd_tpl", "{ \"batteryType\": \"{{ value }}\" }" }});
        json["options"][0] = "Alkali";
        json["options"][1] = "Accumulators";
        json["options"][2] = "Lithium";
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath("select", "battery_type", uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
    else
    {
        removeHassTopic("select", "battery_type", uidString);
    }

    if((int)advancedOpenerConfigAclPrefs[19] == 1)
    {
        // Automatic battery type detection
        publishHassTopic("switch",
                         "automatic_battery_type_detection",
                         uidString,
                         "_automatic_battery_type_detection",
                         "Automatic battery type detection",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_config_advanced_json,
                         deviceType,
                         "",
                         "",
                         "config",
                         String("~") + mqtt_topic_config_action,
                         { { "en", "false" },
                           { "pl_on", "{ \"automaticBatteryTypeDetection\": \"1\"}" },
                           { "pl_off", "{ \"automaticBatteryTypeDetection\": \"0\"}" },
                           { "val_tpl", "{{value_json.automaticBatteryTypeDetection}}" },
                           { "stat_on", "1" },
                           { "stat_off", "0" }});
    }
    else
    {
        removeHassTopic("switch", "automatic_battery_type_detection", uidString);
    }
}

void Network::publishHASSConfigAccessLog(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    publishHassTopic("sensor",
                     "last_action_authorization",
                     uidString,
                     "_last_action_authorization",
                     "Last action authorization",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_log,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "ic", "mdi:format-list-bulleted" },
                                      { "val_tpl", "{{ (value_json|selectattr('type', 'eq', 'LockAction')|selectattr('action', 'in', ['Lock', 'Unlock', 'Unlatch'])|first|default).authorizationName|default }}" }});
}

void Network::publishHASSConfigKeypad(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    // Keypad battery critical
        publishHassTopic("binary_sensor",
                         "keypad_battery_low",
                         uidString,
                         "_keypad_battery_low",
                         "Keypad battery low",
                         name,
                         baseTopic,
                         String("~") + mqtt_topic_battery_keypad_critical,
                         deviceType,
                         "battery",
                         "",
                         "diagnostic",
                         "",
                         {{"pl_on", "1"},
                          {"pl_off", "0"}});

    // Query Keypad
    publishHassTopic("button",
                     "query_keypad",
                     uidString,
                     "_query_keypad_button",
                     "Query keypad",
                     name,
                     baseTopic,
                     "",
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     String("~") + mqtt_topic_query_keypad,
                     { { "en", "false" },
                       { "pl_prs", "1" }});

    publishHassTopic("sensor",
                     "keypad_status",
                     uidString,
                     "_keypad_stats",
                     "Keypad status",
                     name,
                     baseTopic,
                     String("~") + mqtt_topic_lock_log,
                     deviceType,
                     "",
                     "",
                     "diagnostic",
                     "",
                     { { "ic", "mdi:drag-vertical" },
                       { "val_tpl", "{{ (value_json|selectattr('type', 'eq', 'KeypadAction')|first|default).completionStatus|default }}" }});
}

void Network::publishHASSWifiRssiConfig(char *deviceType, const char *baseTopic, char *name, char *uidString)
{
    if(_device->signalStrength() == 127)
    {
        return;
    }

    publishHassTopic("sensor",
                     "wifi_signal_strength",
                     uidString,
                     "_wifi_signal_strength",
                     "WIFI signal strength",
                     name,
                     baseTopic,
                     _lockPath + mqtt_topic_wifi_rssi,
                     deviceType,
                     "signal_strength",
                     "measurement",
                     "diagnostic",
                     "",
                     { {"unit_of_meas", "dBm"} });
}

void Network::publishHassTopic(const String& mqttDeviceType,
                               const String& mqttDeviceName,
                               const String& uidString,
                               const String& uidStringPostfix,
                               const String& displayName,
                               const String& name,
                               const String& baseTopic,
                               const String& stateTopic,
                               const String& deviceType,
                               const String& deviceClass,
                               const String& stateClass,
                               const String& entityCat,
                               const String& commandTopic,
                               std::vector<std::pair<char*, char*>> additionalEntries
)
{
    String discoveryTopic = _preferences->getString(preference_mqtt_hass_discovery);

    if (discoveryTopic != "")
    {
        DynamicJsonDocument json(_bufferSize);
        json = createHassJson(uidString, uidStringPostfix, displayName, name, baseTopic, stateTopic, deviceType, deviceClass, stateClass, entityCat, commandTopic, additionalEntries);
        serializeJson(json, _buffer, _bufferSize);
        String path = createHassTopicPath(mqttDeviceType, mqttDeviceName, uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, _buffer);
    }
}

String Network::createHassTopicPath(const String& mqttDeviceType, const String& mqttDeviceName, const String& uidString)
{
    String discoveryTopic = _preferences->getString(preference_mqtt_hass_discovery);
    String path = discoveryTopic;
    path.concat("/");
    path.concat(mqttDeviceType);
    path.concat("/");
    path.concat(uidString);
    path.concat("/");
    path.concat(mqttDeviceName);
    path.concat("/config");

    return path;
}

void Network::removeHassTopic(const String& mqttDeviceType, const String& mqttDeviceName, const String& uidString)
{
    String discoveryTopic = _preferences->getString(preference_mqtt_hass_discovery);

    if (discoveryTopic != "")
    {
        String path = createHassTopicPath(mqttDeviceType, mqttDeviceName, uidString);
        _device->mqttPublish(path.c_str(), MQTT_QOS_LEVEL, true, "");
    }
}


void Network::removeHASSConfig(char* uidString)
{
    removeHassTopic("lock", "smartlock", uidString);
    removeHassTopic("binary_sensor", "battery_low", uidString);
    removeHassTopic("binary_sensor", "keypad_battery_low", uidString);
    removeHassTopic("sensor", "battery_voltage", uidString);
    removeHassTopic("sensor", "trigger", uidString);
    removeHassTopic("binary_sensor", "mqtt_connected", uidString);
    removeHassTopic("switch", "reset", uidString);
    removeHassTopic("sensor", "firmware_version", uidString);
    removeHassTopic("sensor", "hardware_version", uidString);
    removeHassTopic("sensor", "nuki_hub_version", uidString);
    removeHassTopic("sensor", "nuki_hub_latest", uidString);
    removeHassTopic("update", "nuki_hub_update", uidString);
    removeHassTopic("sensor", "nuki_hub_ip", uidString);
    removeHassTopic("switch", "led_enabled", uidString);
    removeHassTopic("switch", "button_enabled", uidString);
    removeHassTopic("button", "unlatch", uidString);
    removeHassTopic("button", "lockngo", uidString);
    removeHassTopic("button", "lockngounlatch", uidString);
    removeHassTopic("sensor", "battery_level", uidString);
    removeHassTopic("binary_sensor", "door_sensor", uidString);
    removeHassTopic("binary_sensor", "ring", uidString);
    removeHassTopic("number", "led_brightness", uidString);
    removeHassTopic("sensor", "sound_level", uidString);
    removeHassTopic("number", "sound_level", uidString);
    removeHassTopic("sensor", "last_action_authorization", uidString);
    removeHassTopic("sensor", "keypad_status", uidString);
    removeHassTopic("sensor", "wifi_signal_strength", uidString);
    removeHassTopic("sensor", "bluetooth_signal_strength", uidString);
    removeHassTopic("binary_sensor", "continuous_mode", uidString);
    removeHassTopic("switch", "continuous_mode", uidString);
    removeHassTopic("button", "query_lockstate", uidString);
    removeHassTopic("button", "query_config", uidString);
    removeHassTopic("button", "query_keypad", uidString);
    removeHassTopic("button", "query_battery", uidString);
    removeHassTopic("button", "query_commandresult", uidString);
    removeHassTopic("switch", "auto_lock", uidString);
    removeHassTopic("switch", "auto_unlock", uidString);
    removeHassTopic("switch", "double_lock", uidString);
}

void Network::removeHASSConfigTopic(char *deviceType, char *name, char *uidString)
{
    removeHassTopic(deviceType, name, uidString);
}

DynamicJsonDocument Network::createHassJson(const String& uidString,
                             const String& uidStringPostfix,
                             const String& displayName,
                             const String& name,
                             const String& baseTopic,
                             const String& stateTopic,
                             const String& deviceType,
                             const String& deviceClass,
                             const String& stateClass,
                             const String& entityCat,
                             const String& commandTopic,
                             std::vector<std::pair<char*, char*>> additionalEntries
)
{
    DynamicJsonDocument json(_bufferSize);

    json.clear();
    auto dev = json.createNestedObject("dev");
    auto ids = dev.createNestedArray("ids");
    ids.add(String("nuki_") + uidString);
    json["dev"]["mf"] = "Nuki";
    json["dev"]["mdl"] = deviceType;
    json["dev"]["name"] = name;
    json["~"] = baseTopic;
    json["name"] = displayName;
    json["unique_id"] = String(uidString) + uidStringPostfix;

    if(deviceClass != "")
    {
        json["dev_cla"] = deviceClass;
    }

    if(stateTopic != "")
    {
        json["stat_t"] = stateTopic;
    }

    if(stateClass != "")
    {
        json["stat_cla"] = stateClass;
    }

    if(entityCat != "")
    {
        json["ent_cat"] = entityCat;
    }

    if(commandTopic != "")
    {
        json["cmd_t"] = commandTopic;
    }

    json["avty"]["t"] = _lockPath + mqtt_topic_mqtt_connection_state;

    for(const auto& entry : additionalEntries)
    {
        if(strcmp(entry.second, "true") == 0)
        {
            json[entry.first] = true;
        }
        else if(strcmp(entry.second, "false") == 0)
        {
            json[entry.first] = false;
        }
        else
        {
            json[entry.first] = entry.second;
        }
    }

    return json;
}

void Network::publishPresenceDetection(char *csv)
{
    _presenceCsv = csv;
}

const NetworkDeviceType Network::networkDeviceType()
{
    return _networkDeviceType;
}

uint16_t Network::subscribe(const char *topic, uint8_t qos)
{
    return _device->mqttSubscribe(topic, qos);
}

void Network::setKeepAliveCallback(std::function<void()> reconnectTick)
{
    _keepAliveCallback = reconnectTick;
}

void Network::addReconnectedCallback(std::function<void()> reconnectedCallback)
{
    _reconnectedCallbacks.push_back(reconnectedCallback);
}

void Network::clearWifiFallback()
{
    memset(WiFi_fallbackDetect, 0, sizeof(WiFi_fallbackDetect));
}

void Network::disableMqtt()
{
    _device->disableMqtt();
    _mqttEnabled = false;
}

NetworkDevice *Network::device()
{
    return _device;
}
