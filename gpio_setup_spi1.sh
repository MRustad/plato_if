#!/bin/bash -e
# GPIO setup script for SPI1 on Galileo for Linux access
# Mark Rustad, mark.d.rustad@intel.com

gpio_path=/sys/class/gpio

gpio4="out 1"
gpio42="out 0 strong"
gpio43="out 0 strong"
gpio54="out 0 strong"
gpio55="out 0 strong"

gpios="4 42 43 54 55"

setup_gpio() {
	echo "Gpio ${1}: direction: $2, value: $3, drive: $4"
	echo -n $2 > $1/direction
	echo -n $3 > $1/value
	echo -n $4 > $1/drive
}

for i in ${gpios}; do
	gpio=${gpio_path}/gpio${i}
	if [ ! -d ${gpio} ]; then
		echo "Exporting gpio ${i}"
		echo -n ${i} > ${gpio_path}/export
	fi
done

for i in ${gpios}; do
	gpio=${gpio_path}/gpio${i}
	if [ -d ${gpio} ]; then
		gpiovar=gpio${i}
		setup_gpio ${gpio} ${!gpiovar}
	fi
done
