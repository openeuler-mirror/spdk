#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021-2025 Huawei Technologies Co.
#  All rights reserved.
#

import argparse
import logging
import sys
import os
import stat
import pwd
import grp
import json

sys.path.append(os.path.dirname(__file__) + '/../python')

import spdk.rpc as rpc  # noqa
from spdk.rpc.client import print_dict, JSONRPCException  # noqa
from spdk.rpc.helpers import deprecated_aliases  # noqa


def get_parser():
    parser = argparse.ArgumentParser(
        description='SPDK RPC command line interface', usage='%(prog)s [options]', add_help=False)

    parser.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    parser.add_argument('-r', dest='conn_retries',
                        help='Retry connecting to the RPC server N times with 0.2s interval. Default: 0',
                        default=0, type=int)
    parser.add_argument('-t', dest='timeout',
                        help='Timeout as a floating point number expressed in seconds, waiting for response. Default: 60.0',
                        default=60.0, type=float)
    parser.add_argument('-s', dest='server_addr',
                         help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
    parser.set_defaults(is_server=False)
    parser.set_defaults(dry_run=False)
    parser.set_defaults(port=5260)
    parser.set_defaults(verbose="ERROR")
    return parser

def change_queues_num(client, number):
    if not (1 <= number <= 32):
        print("the number is not legal, it should be 1 <= number <= 32")
        return
    path = "/etc/dpak/ssam/parameter.json"
    with open(path, 'r') as file:
        try:
            data = json.load(file)
        except json.JSONDecodeError:
            print("JSON file is wrong")
            return

    if "queues" not in data:
        print("JSON file do not have 'queues'")
        return

    data["queues"] = number
    with open(path, 'w') as file:
        json.dump(data, file, indent=4)


def init_rpc_func():
    parser = get_parser()
    subparsers = parser.add_subparsers(help='RPC methods', dest='called_rpc_name', metavar='')

    @rpc.ssam.log_info
    def create_blk_controller(args):
        rpc.ssam.create_blk_controller(args.client,
                                       dev_name=args.dev_name,
                                       index=args.index,
                                       readonly=args.readonly,
                                       serial=args.serial,
                                       vqueue=args.vqueue)

    p = subparsers.add_parser('create_blk_controller',
                              help='Add a new block controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('dev_name', help='Name of block device')
    p.add_argument('index', help='Function ID or dbdf')
    p.add_argument("-r", "--readonly", action='store_true', help='Set controller as read-only')
    p.add_argument("-s", "--serial", help='Set volume ID')
    p.add_argument("-q", "--vqueue", help='Set virtio queue num with a range of [1, 32]', type=int, required=False)
    p.set_defaults(func=create_blk_controller)

    @rpc.ssam.log_info
    def get_controllers(args):
        print_dict(rpc.ssam.get_controllers(args.client, args.function_id, args.dbdf))

    p = subparsers.add_parser('get_controllers',
                              help='List all or specific controller(s)', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('-f', '--function_id', help="Function ID of PCI device", type=int, required=False)
    p.add_argument('-d', '--dbdf', help="Dbdf of PCI device", required=False)
    p.set_defaults(func=get_controllers)

    @rpc.ssam.log_info
    def get_scsi_controllers(args):
        print_dict(rpc.ssam.get_scsi_controllers(args.client, args.name))

    p = subparsers.add_parser('get_scsi_controllers', aliases=['scsi_controller_list'],
                              help='List all or specific scsi controller(s)', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('-n', '--name', help="Name of controller", required=False)
    p.set_defaults(func=get_scsi_controllers)

    @rpc.ssam.log_info
    def delete_controller(args):
        rpc.ssam.delete_controller(args.client,
                                   index=args.index,
                                   force=args.force)

    p = subparsers.add_parser('delete_controller',
                              help='Delete a controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('index', help='Function ID or dbdf of PCI device')
    p.add_argument('-f', '--force', dest='force', action='store_true', help="Force to delete when io exists")
    p.set_defaults(force=False)
    p.set_defaults(func=delete_controller)

    @rpc.ssam.log_info
    def delete_scsi_controller(args):
        rpc.ssam.delete_scsi_controller(args.client, name=args.name)

    p = subparsers.add_parser('delete_scsi_controller', aliases=['scsi_controller_delete'],
                              help='Delete a scsi controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of controller to be deleted', type=str)
    p.set_defaults(func=delete_scsi_controller)

    @rpc.ssam.log_info
    def bdev_resize(args):
        rpc.ssam.bdev_resize(args.client,
                             function_id=args.function_id,
                             new_size_in_mb=args.new_size_in_mb)

    p = subparsers.add_parser('bdev_resize',
                              help='Resize a blk bdev by blk controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('function_id', help='Function ID of PCI device', type=int)
    p.add_argument('new_size_in_mb', help='New size of bdev for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=bdev_resize)

    @rpc.ssam.log_info
    def scsi_bdev_resize(args):
        rpc.ssam.scsi_bdev_resize(args.client,
                                  name=args.name,
                                  tgt_id=args.tgt_id,
                                  new_size_in_mb=args.new_size_in_mb)

    p = subparsers.add_parser('scsi_bdev_resize',
                              help='Resize a scsi bdev by scsi controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of controller for the PCI device', type=str)
    p.add_argument('tgt_id', help='Tgt ID of bdev', type=int)
    p.add_argument('new_size_in_mb', help='New size of bdev for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=scsi_bdev_resize)

    @rpc.ssam.log_info
    def bdev_aio_resize(args):
        rpc.ssam.bdev_aio_resize(args.client,
                                 name=args.name,
                                 new_size_in_mb=args.new_size_in_mb)

    p = subparsers.add_parser('bdev_aio_resize',
                              help='Resize a bdev by bdev name', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of aio bdev', type=str)
    p.add_argument('new_size_in_mb', help='New size of bdev for resize operation. The unit is MiB', type=int)
    p.set_defaults(func=bdev_aio_resize)

    @rpc.ssam.log_info
    def set_queues_num(args):
        change_queues_num(args.client,
                          number=args.number)
    p = subparsers.add_parser('set_queues_num',
                              help='Set the queues of translate', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('number', help='Number of queues', type=int)
    p.set_defaults(func=set_queues_num)

    @rpc.ssam.log_info
    def os_ready(args):
        rpc.ssam.os_ready(args.client)

    p = subparsers.add_parser('os_ready',
                              help='Write ready flag for booting OS', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=os_ready)

    @rpc.ssam.log_info
    def os_not_ready(args):
        rpc.ssam.os_not_ready(args.client)

    p = subparsers.add_parser('os_not_ready',
                              help='Write not ready flag for booting OS', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=os_not_ready)

    @rpc.ssam.log_info
    def controller_get_iostat(args):
        print_dict(rpc.ssam.controller_get_iostat(args.client, args.function_id, args.dbdf))

    p = subparsers.add_parser('controller_get_iostat',
                              help='Show all or specific controller(s) iostat', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('-f', '--function_id', help="Function ID of PCI device", type=int, required=False)
    p.add_argument('-d', '--dbdf', help="Dbdf of PCI device", required=False)
    p.set_defaults(func=controller_get_iostat)

    @rpc.ssam.log_info
    def blk_device_iostat(args):
        print_dict(rpc.ssam.blk_device_iostat(args.client,
                                              index=args.index,
                                              tid=args.tid,
                                              vq_idx=args.vq_idx))

    p = subparsers.add_parser('blk_device_iostat',
                              help='Show iostat of blk device', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('index', help='Function ID or dbdf')
    p.add_argument('-t', "--tid", help='Tid', type=int, required=False)
    p.add_argument("-q", "--vq_idx", help='Index of vqueue', type=int, required=False)
    p.set_defaults(func=blk_device_iostat)

    @rpc.ssam.log_info
    def controller_clear_iostat(args):
        rpc.ssam.controller_clear_iostat(args.client)

    p = subparsers.add_parser('controller_clear_iostat',
                              help='Clear all controllers iostat', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=controller_clear_iostat)

    @rpc.ssam.log_info
    def create_scsi_controller(args):
        rpc.ssam.create_scsi_controller(args.client,
                                        dbdf=args.dbdf,
                                        name=args.name)

    p = subparsers.add_parser('create_scsi_controller', aliases=['scsi_controller_create'],
                              help='Add a new scsi controller', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('dbdf', help='The pci dbdf of virtio scsi controller, which is obtained by \'device_pcie_list\'', type=str)
    p.add_argument('name', help='Name of controller to be created', type=str)
    p.set_defaults(func=create_scsi_controller)

    @rpc.ssam.log_info
    def scsi_controller_add_target(args):
        rpc.ssam.scsi_controller_add_target(args.client,
                                            name=args.name,
                                            scsi_tgt_num=int(args.scsi_tgt_num),
                                            bdev_name=args.bdev_name)

    p = subparsers.add_parser('scsi_controller_add_target',
                              help='Add LUN to ssam scsi controller target', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of controller where lun is added', type=str)
    p.add_argument('scsi_tgt_num', help='ID of target to use')
    p.add_argument('bdev_name', help='Name of bdev to be added to target')
    p.set_defaults(func=scsi_controller_add_target)

    @rpc.ssam.log_info
    def scsi_controller_remove_target(args):
        rpc.ssam.scsi_controller_remove_target(args.client,
                                               name=args.name,
                                               scsi_tgt_num=int(args.scsi_tgt_num))

    p = subparsers.add_parser('scsi_controller_remove_target',
                              help='Remove LUN from ssam scsi controller target', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of controller to remove lun', type=str)
    p.add_argument('scsi_tgt_num', help='ID of target to use')
    p.set_defaults(func=scsi_controller_remove_target)

    @rpc.ssam.log_info
    def scsi_device_iostat(args):
        print_dict(rpc.ssam.scsi_device_iostat(args.client,
                                               name=args.name,
                                               scsi_tgt_num=int(args.scsi_tgt_num)))

    p = subparsers.add_parser('scsi_device_iostat',
                              help='Show iostat of scsi device', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('name', help='Name of controller', type=str)
    p.add_argument('scsi_tgt_num', help='ID of target', type=int)
    p.set_defaults(func=scsi_device_iostat)

    @rpc.ssam.log_info
    def device_pcie_list(args):
        print_dict(rpc.ssam.device_pcie_list(args.client))

    p = subparsers.add_parser('device_pcie_list',
                              help='Show storage device pcie list', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=device_pcie_list)

    @rpc.ssam.log_info
    def controller_function_get_qos_limit(args):
        print_dict(rpc.ssam.controller_function_get_qos_limit(args.client,
                                        args.function_id))

    p = subparsers.add_parser('controller_function_get_qos_limit',
                                help='Get QoS rate limit for a function', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--function_id',
                    help='function to configure.',
                    type=int, required=True)
    p.set_defaults(func=controller_function_get_qos_limit)

    @rpc.ssam.log_info
    def controller_function_set_qos_limit(args):
        iops_qos_info = []
        bw_qos_info = []
        if args.rw_ios_per_sec:
            for i in args.rw_ios_per_sec.strip().split(','):
                iops_qos_info.append(i)
        if args.rw_mbits_per_sec:
            for n in args.rw_mbits_per_sec.strip().split(','):
                bw_qos_info.append(n)
        rpc.ssam.controller_function_set_qos_limit(args.client,
                                        args.function_id,
                                        rw_ios_per_sec=iops_qos_info,
                                        rw_mbits_per_sec=bw_qos_info)

    p = subparsers.add_parser('controller_function_set_qos_limit',
                                help='Set QoS rate limit for a function', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--function_id',
                    help='function to configure.',
                    type=int, required=True)
    p.add_argument('--rw-ios-per-sec',
                    help='R/W IOs per second limit (cir,cbs,pir,pbs). '
                    'cir range [1000, pir] iops, pir range [cir, 128000000) iops. '
                    'cbs range [1000, pbs] ios, pbs range [cbs, 1000000] ios. '
                    '(0,0,0,0) means unlimited. '
                    'The step of cir/pir is 1000.',
                    required=False)
    p.add_argument('--rw-mbits-per-sec',
                    help='R/W megabits per second limit (cir,cbs,pir,pbs). '
                    'cir range [1, pir] Mbps,  pir range [cir, 400000) Mbps. '
                    'cbs range [1, pbs] Mbits, pbs range [cbs, 2560] Mbits. '
                    '(0,0,0,0) means unlimited. '
                    'When cir/pir in [1, 1000] Mbps, the step is 1M. While in (1000, 400000) Mbps, the step is 100M.',
                    required=False)
    p.set_defaults(func=controller_function_set_qos_limit)

    @rpc.ssam.log_info
    def controller_create_group_qos_limit(args):
        rpc.ssam.controller_create_group_qos_limit(args.client,
                                        args.group_name)
    
    p = subparsers.add_parser('controller_create_group_qos_limit',
                                help='Create QoS rate limit for a group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_create_group_qos_limit)

    @rpc.ssam.log_info
    def controller_delete_group_qos_limit(args):
        rpc.ssam.controller_delete_group_qos_limit(args.client,
                                        args.group_name)
    
    p = subparsers.add_parser('controller_delete_group_qos_limit',
                                help='Delete a QoS group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_delete_group_qos_limit)

    @rpc.ssam.log_info
    def controller_qos_add_function_to_group(args):
        rpc.ssam.controller_qos_add_function_to_group(args.client,
                                        args.group_name,
                                        args.function_id)

    p = subparsers.add_parser('controller_qos_add_function_to_group',
                                help='Add function to a QoS group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--function_id',
                    help='function to be added to a QoS group.',
                    type=int, required=True)
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_qos_add_function_to_group)


    @rpc.ssam.log_info
    def controller_qos_delete_function_from_group(args):
        rpc.ssam.controller_qos_delete_function_from_group(args.client,
                                        args.group_name,
                                        args.function_id)

    p = subparsers.add_parser('controller_qos_delete_function_from_group',
                                help='Delete a function from a QoS group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--function_id',
                    help='function to be deleted from a QoS group.',
                    type=int, required=True)
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_qos_delete_function_from_group)


    @rpc.ssam.log_info
    def controller_qos_get_function_of_group(args):
        print_dict(rpc.ssam.controller_qos_get_function_of_group(args.client,
                                        args.group_name))

    p = subparsers.add_parser('controller_qos_get_function_of_group',
                                help='Get a function list in a QoS group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_qos_get_function_of_group)


    @rpc.ssam.log_info
    def controller_group_set_qos_limit(args):
        iops_qos_info = []
        bw_qos_info = []
        if args.rw_ios_per_sec:
            for i in args.rw_ios_per_sec.strip().split(','):
                iops_qos_info.append(i)
        if args.rw_mbits_per_sec:
            for n in args.rw_mbits_per_sec.strip().split(','):
                bw_qos_info.append(n)
        rpc.ssam.controller_group_set_qos_limit(args.client,
                                                args.group_name,
                                                rw_ios_per_sec=iops_qos_info,
                                                rw_mbits_per_sec=bw_qos_info)
    
    p = subparsers.add_parser('controller_group_set_qos_limit',
                                help='Set QoS rate limit for a group', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.add_argument('--rw-ios-per-sec',
                    help='R/W IOs per second limit (cir,cbs,pir,pbs). '
                    'cir range [1000, pir] iops, pir range [cir, 128000000) iops. '
                    'cbs range [1000, pbs] ios, pbs range [cbs, 1000000] ios. '
                    '(0,0,0,0) means unlimited. '
                    'The step of cir/pir is 1000.',
                    required=False)
    p.add_argument('--rw-mbits-per-sec',
                    help='R/W megabits per second limit (cir,cbs,pir,pbs). '
                    'cir range [1, pir] Mbps,  pir range [cir, 400000) Mbps. '
                    'cbs range [1, pbs] Mbits, pbs range [cbs, 2560] Mbits. '
                    '(0,0,0,0) means unlimited. '
                    'When cir/pir in [1, 1000] Mbps, the step is 1M. While in (1000, 400000) Mbps, the step is 100M.',
                    required=False)
    p.set_defaults(func=controller_group_set_qos_limit)
    
    @rpc.ssam.log_info
    def controller_group_get_qos_limit(args):
        print_dict(rpc.ssam.controller_group_get_qos_limit(args.client,
                                        args.group_name))

    p = subparsers.add_parser('controller_group_get_qos_limit',
                                help='Get QoS rate limit for a group', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('--group_name',
                    help='name of the group.',
                    type=str, required=True)
    p.set_defaults(func=controller_group_get_qos_limit)


    @rpc.ssam.log_info
    def controller_host_set_qos_limit(args):
        iops_qos_info = []
        bw_qos_info = []
        if args.rw_ios_per_sec:
            for i in args.rw_ios_per_sec.strip().split(','):
                iops_qos_info.append(i)
        if args.rw_mbits_per_sec:
            for n in args.rw_mbits_per_sec.strip().split(','):
                bw_qos_info.append(n)
        rpc.ssam.controller_host_set_qos_limit(args.client,
                                       rw_ios_per_sec=iops_qos_info,
                                       rw_mbits_per_sec=bw_qos_info)

    p = subparsers.add_parser('controller_host_set_qos_limit',
                              help='Set QoS rate limit for host', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.add_argument('--rw-ios-per-sec',
                   help='R/W IOs per second limit (cir,cbs). '
                   'cir range [1000, 128000000) iops. '
                   'cbs range [1000, 1000000] ios. '
                   '(0,0) means unlimited. '
                   'The step of cir is 1000.',
                   required=False)
    p.add_argument('--rw-mbits-per-sec',
                   help='R/W megabits per second limit (cir,cbs). '
                   'cir range [1, 400000) Mbps. '
                   'cbs range [1, 2560] Mbits. '
                   '(0,0) means unlimited. '
                   'When cir in [1, 1000] Mbps, the step is 1M. While in (1000, 400000) Mbps, the step is 100M.',
                   required=False)
    p.set_defaults(func=controller_host_set_qos_limit)

    @rpc.ssam.log_info
    def controller_host_get_qos_limit(args):
        print_dict(rpc.ssam.controller_host_get_qos_limit(args.client))

    p = subparsers.add_parser('controller_host_get_qos_limit',
                                help='Get QoS rate limit for Host', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.set_defaults(func=controller_host_get_qos_limit)

    @rpc.ssam.log_info
    def controller_clear_qos_limit(args):
        print_dict(rpc.ssam.controller_clear_qos_limit(args.client))

    p = subparsers.add_parser('controller_clear_qos_limit',
                                help='Clear hardware QoS rate limit', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.set_defaults(func=controller_clear_qos_limit)

    @rpc.ssam.log_info
    def hot_upgrade_start_io_poll(args):
        print_dict(rpc.ssam.hot_upgrade_start_io_poll(args.client))

    p = subparsers.add_parser('hot_upgrade_start_io_poll',
                              help='start io poll ', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=hot_upgrade_start_io_poll)

    @rpc.ssam.log_info
    def check_hot_upgrade_status(args):
        print_dict(rpc.ssam.check_hot_upgrade_status(args.client))

    p = subparsers.add_parser('check_hot_upgrade_status',
                              help='check process hot upgrade status', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=check_hot_upgrade_status)

    @rpc.ssam.log_info
    def get_ssam_info(args):
        print_dict(rpc.ssam.get_ssam_info(args.client))
    p = subparsers.add_parser('get_ssam_info',
                              help='Get ssam information', add_help=False)
    p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
    p.set_defaults(func=get_ssam_info)

    @rpc.ssam.log_info
    def set_crc_checklog(args):
        rpc.ssam.set_crc_checklog(args.client, toggle=args.toggle)
    p = subparsers.add_parser('set_crc_checklog',
                              help='Set IO CRC Check Log', add_help=False)
    p.add_argument('-h', '--help', action='help', help='Show this help message and exit')
    p.add_argument('toggle', help='set io crc check enable or disable')
    p.set_defaults(func=set_crc_checklog)

    return parser


if __name__ == "__main__":
    def call_rpc_func(args):
        args.func(args)
        check_called_name(args.called_rpc_name)

    def check_called_name(name):
        if name in deprecated_aliases:
            print("{} is deprecated, use {} instead.".format(name, deprecated_aliases[name]), file=sys.stderr)

    parser = init_rpc_func()
    args = parser.parse_args()

    if sys.stdin.isatty() and not hasattr(args, 'func'):
        # No arguments and no data piped through stdin
        parser.print_help()
        exit(1)

    if args.called_rpc_name != "get_version":
        args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout,
                                               log_level=getattr(logging, args.verbose.upper()),
                                               conn_retries=args.conn_retries)

    try:
        call_rpc_func(args)
    except JSONRPCException as ex:
        print(ex.message)
        exit(1)
