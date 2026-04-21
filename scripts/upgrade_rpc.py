#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021-2025 Huawei Technologies Co.
#  All rights reserved.
#

import argparse
import json
import socket
from getpass import getuser

try:
    from shlex import quote
except ImportError:
    from pipes import quote

def print_dict(d):
    print(json.dumps(d, indent=2))

def print_array(a):
    print(" ".join((quote(v) for v in a)))

parser = argparse.ArgumentParser(
        description='SPDK UPGRADE RPC command line interface', usage='%(prog)s [options]', add_help=False)
parser.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
parser.add_argument('-s', dest='server_addr',
                        help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
subparsers = parser.add_subparsers(help='RPC methods')
class JSONRPCException(Exception):
    def __init__(self, message):
        self.message = message

def int_arg(arg):
    return int(arg, 0)

def jsonrpc_call(method, params={}, timeout=60):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(args.server_addr)
    except socket.error as ex:
            raise JSONRPCException("Error while connecting to %s\n"
                                   "Is SPDK application running?\n"
                                   "Error details: %s" % (args.server_addr, ex))
    req = {}
    req['jsonrpc'] = '2.0'
    req['method'] = method
    req['id'] = 2
    if (params):
        req['params'] = params
    reqstr = json.dumps(req)
    
    s.sendall(reqstr.encode())
    buf=''
    closed = False 
    response = {}
    while not closed:
        newdata = s.recv(4096)
        if (newdata == b''):
            closed = True
        buf += newdata.decode()
        try:
            response = json.loads(buf)
        except ValueError:
            continue 
        break
    s.close()

    if not response:
        print(" Connection closed with partial response:")
        print(buf)
        exit(1)

    if 'error' in response:
        print("request:")
        print_dict(json.loads(reqstr))
        print("Got JSON-RPC error response")
        print("response:")
        print_dict(response['error'])
        exit(1)

    return response['result']

def log_info(method):
    params = {
        'user_name': getuser(),
        'event': method,
        'src_addr': "localhost",
    }
    jsonrpc_call('log_command_info', params)

def hot_upgrade_start_io_poll(args):
        params = {}
        log_info('hot_upgrade_start_io_poll')
        print_dict(jsonrpc_call('hot_upgrade_start_io_poll', params))

p = subparsers.add_parser('hot_upgrade_start_io_poll',
                              help='start io poll ', add_help=False)
p.add_argument('-h', '--help',  action='help', help='Show this help message and exit')
p.set_defaults(func=hot_upgrade_start_io_poll)

args = parser.parse_args()
args.func(args)