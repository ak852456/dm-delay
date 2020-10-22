# dm-delay

//create 2G space ram disk

sudo modprobe brd rd_nr=1 rd_size=2097152 max_part=0

sudo chmod 777 /dev/ram0

/******************** use ram0 as swap area ********************/

sudo mkswap /dev/ram0

sudo swapon -p 0 /dev/ram0

sudo sysctl vm.swappiness=100

sudo swapoff -a

/**************************************************************/

# 1. read device
# 2. read start address
# 3. read delay (microseconds)
# 4. write device
# 5. write start address
# 6. write delay (microseconds)

//delay read 300us and write 500us 

sudo echo "0 `blockdev --getsz /dev/ram0` delay /dev/ram0 0 300 /dev/ram0 0 500" | dmsetup create delayed 

dmsetup remove /dev/dm-0