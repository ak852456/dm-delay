#!/bin/bash

sudo modprobe brd rd_nr=1 rd_size=2097152 max_part=0 # create 2G space ram disk

sudo chmod 777 /dev/ram0

sudo mkswap /dev/ram0

sudo swapon -p 0 /dev/ram0

#sudo sysctl vm.swappiness=100

#sudo swapoff -a

# delay read 300us and write 500us 
sudo echo "0 `blockdev --getsz /dev/ram0` delay /dev/ram0 0 300 /dev/ram0 0 500" | dmsetup create delayed 

#dmsetup remove /dev/dm-0 

#systemd-run -t -p MemoryLimit=50M bash -c "cd $(pwd); source env.sh; time parsecmgmt -a run -p dedup -i native"    // parsec-3.0

#systemd-run -t -p MemoryLimit=50M bash -c "cd $(pwd); source shrc; time runcpu --config=cfwu --size=ref --copies=1 --iterations=1 526.blender_r" // cpubenchmark2017

