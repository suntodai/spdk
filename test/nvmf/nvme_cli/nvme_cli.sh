#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

set -e

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter nvme_cli

$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 5

modprobe -v nvme-rdma

if [ -e "/dev/nvme-fabrics" ]; then
	chmod a+rw /dev/nvme-fabrics
fi

echo 'traddr='$NVMF_FIRST_TARGET_IP',transport=rdma,nr_io_queues=1,trsvcid='$NVMF_PORT',nqn=nqn.2016-06.io.spdk:cnode1' > /dev/nvme-fabrics

nvme list
nvme id-ctrl /dev/nvme0
nvme id-ns /dev/nvme0n1
nvme smart-log /dev/nvme0

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit nvme_cli
