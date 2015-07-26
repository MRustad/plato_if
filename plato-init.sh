#!/bin/bash

# Initialization script for platod. Sets up gpio for the platform
# and puts a message on the terminal.
# Mark Rustad, mark.d.rustad@intel.com

gpio_path=/sys/class/gpio
ulb=/usr/local/bin

sleep 5

board=$(cat /sys/devices/virtual/dmi/id/board_name)
case "$board" in
*"GalileoGen2" )
	gpio=(	24 "out 0 strong"
		25 "out 1 strong"
		44 "out 1 strong"
		72 "out 0"
		42 "out 1 strong"
		43 "out 1 strong"
		30 "out 0 strong"
		31 "out 1 strong"
		46 "out 1 strong"
	)
	;;
*"Galileo" )
	gpio=(  4 "out 1"
		42 "out 0 strong"
		43 "out 0 strong"
		54 "out 0 strong"
		55 "out 0 strong"
	)
	;;
* )
	echo "Unknown board ${board}"
	exit 1
	;;
esac

export_gpios() {
	while [ -n "${1}" ]; do
		if [ ! -d ${gpio_path}/gpio${1} ]; then
			echo "Exporting gpio ${1}"
			echo -n ${1} > ${gpio_path}/export
		fi
		shift
		shift
	done
}

set_gpio() {
	if [ -e ${1} ]; then
		echo -n ${2} > ${1} || echo "Setting ${1} failed"
	else
		echo "No ${1##*/} for ${1%/*}"
	fi
}

setup_gpio() {
	echo "${board}: Gpio ${1}: direction: ${2}, value: ${3}, drive: ${4}"
	set_gpio ${1}/direction ${2}
	test -n "${3}" && set_gpio ${1}/value ${3}
	test -n "${4}" && set_gpio ${1}/drive ${4}
}

setup_gpios() {
	while [ -n "$1" ]; do
		gpio_dir=${gpio_path}/gpio$1
		test -d ${gpio_dir} && setup_gpio ${gpio_dir} $2
		shift
		shift
	done
}

export_gpios "${gpio[@]}"
setup_gpios "${gpio[@]}"

${ulb}/platomsg -c "Starting up"
