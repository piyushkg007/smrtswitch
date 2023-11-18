# Doorbell camera example

This example is a simple door bell camera application based on TensorFlow's standard person-detection example. When a person is detected by the camera (note detected, not identified), then an email is sent with the captured image to a configured email address. This example uses a 250KB neural network to detect people in images captured by the camera.

## Table of contents
-  [The ESP-EYE](#the-esp-eye)
-  [Running on the ESP-EYE](#running-on-the-esp-eye)
-  [Using a dev-kit other than ESP-EYE](#using-a-dev-kit-other-than-esp-eye)

## The ESP-EYE
The [ESP-EYE](https://www.espressif.com/en/products/devkits/esp-eye/overview) is a development board for image recognition and audio processing, which can be used in various AIoT applications. It features an ESP32 chip, a 2-Megapixel camera and a microphone. ESP-EYE offers plenty of storage, with an 8 Mbyte PSRAM and a 4 Mbyte flash. It also supports Wi-Fi/BT/BLE transports and debuggability through a Micro-USB port.<br/>
<img src="https://raw.githubusercontent.com/wiki/espressif/tensorflow/images/ESP-EYE-Final.jpg" alt="ESP-EYE" align="middle" width="400"/>

## Running on the ESP-EYE

### Install the ESP IDF

Follow the instructions of the
[ESP-IDF get started guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html)
to setup the toolchain and the ESP-IDF itself.

The next steps assume that the
[IDF environment variables are set](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html#step-4-set-up-the-environment-variables) :

*   The `IDF_PATH` environment variable is set
*   `idf.py` and Xtensa-esp32 tools (e.g. `xtensa-esp32-elf-gcc`) are in `$PATH`
*   `esp32-camera` should be downloaded in `components/` dir of example as
    explained in `Configure the example`(below)

### Generate the example

The example project can be generated with the following command:
```
make -f tensorflow/lite/micro/tools/make/Makefile TARGET=esp generate_doorbell_camera_esp_project
```

  - On mac I have seen issues with `make`. Please use `gmake` if you happen to be so.
> Please ignore few warnings raised by this command.

### Configure the example

Go the the example project directory

```
cd tensorflow/lite/micro/tools/make/gen/esp_xtensa-esp32/prj/doorbell_camera/esp-idf
```

As the `doorbell_camera` example requires an external component `esp32-camera`
for functioning hence we will have to manually clone it in `components/`
directory of the example with following commands.
```
git clone https://github.com/espressif/esp32-camera.git components/esp32-camera
cd components/esp32-camera && git checkout eacd640b8d379883bff1251a1005ebf3cf1ed95c && cd ../../
```

To configure the camera and email settings, run
```idf.py menuconfig```

We will perform the following 2 configurations:
* *Wi-Fi Configuration:* Wifi or Ethernet can be configured under the "Example Connection Configuration" menu. It includes the following two things,
  * *SSID* - SSID of the wifi network to which ESP device is to be connected.
  * *Password* - Password of the wifi network.

* *SMTP Configuration:* The SMTP configuration can be done under the "SMTP Configuration" menu. It includes setup of the following things,
  * Mail server - the mail server to be used by the SMTP client(the default is `smtp.googlemail.com`)
     * By default CA certificate of `smtp.googlemail.com` is provided in SMTP client component, you will also need to update the CA certificate if you wish to change the mail server
  * Mail port number - the SMTP port number for the example to connect to ( default is `587` )
  * Sender email - Sender's email address
  * Sender password - Sender's email password that will be used for authentication with the SMTP server above
  * Recipient email - The email address to which the images will be sent

[If you are using your GMail address to send out email, the ESP32 will be signing into your Gmail on your behalf with a simple SMTP Client. Please visit the following URL to enable this access: [Less secure apps & your Google Account](https://support.google.com/accounts/answer/6010255).

### Build the example

Then build with `idf.py`:
```
idf.py build
```

### Load and run the example

To flash (replace `/dev/ttyUSB0` with the device serial port):
```
idf.py --port /dev/ttyUSB0 flash
```
Monitor the serial output:
```
idf.py --port /dev/ttyUSB0 monitor
```
Use `Ctrl+]` to exit.

When a person is detected (One inference is done in about 1 second), the captured image is sent to the email address using the configuration provided in the build step.

## Using a dev-kit other than ESP-EYE

If you want to use other ESP Dev-Kits you will have to connect a camera externally to them, and then write your own
[image_provider.cc](https://github.com/espressif/tensorflow/tree/master/tensorflow/lite/micro/examples/doorbell_camera/esp/image_provider.cc).
and
[app_camera_esp.c](https://github.com/espressif/tensorflow/tree/master/tensorflow/lite/micro/examples/doorbell_camera/esp/app_camera_esp.c).
You can also write you own
[detection_responder.cc](https://github.com/espressif/tensorflow/tree/master/tensorflow/lite/micro/examples/doorbell_camera/detection_responder.cc).
