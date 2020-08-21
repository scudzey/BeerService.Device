# BeerIoTTapSensor

This repository contains the firmware defintion for an ESP32 monitoring flowsensors for a beer monitoring service.  The sensors all provide a pulse signal on a designated pin that triggers an interrupt that will increment a counter for each tap.  Periodically when available, the main processing loop will ingest the update messages and publish each of the Tap values on a MQTT topic to the configured AWS account.

### Dependencies
This device is build upon the ESP-IDF library using their esp-aws-iot component package.

[Espressif ESP32 package](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)  
[Espressif AWS IoT Core](https://github.com/espressif/esp-aws-iot)


### Setup
This requires the configuration of an AWS IoT Core thing, along with a private key associated to a policy.  These keys will go into the main/certs directory when compiling.

* run `idf.py menuconfig`
* Configure the AWS IoT 
    * Component config -> Amazon Web Services IoT Platform
    * Set AWS IoT Endpoint Hostname
* Configure `Device Flow Configuartion`
    * Set SSID for the device to connect to
    * Set the WiFi password for the SSID
    * Set the AWS IoT Thing name
    * Set the AWS IoT Client ID (this is usually the same as the Thing name)

When build after running the menuconfig, the values configured will produce a configuration header file.

