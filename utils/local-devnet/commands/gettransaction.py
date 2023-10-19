#!/usr/bin/python3

import sys
sys.path.append('../testdata')
import config

import requests
import argparse
import json


def instruct_daemon(method, params):
    payload = json.dumps({"method": method, "params": params}, skipkeys=False)
    print(payload)
    headers = {'content-type': "application/json"}
    try:
        response = requests.request("POST", "http://"+config.listen_ip+":"+config.listen_port+"/json_rpc", data=payload, headers=headers)
        return json.loads(response.text)
    except requests.exceptions.RequestException as e:
        print(e)
    except:
        print('No response from daemon, check daemon is running on this machine')


parser = argparse.ArgumentParser(description='Get Block.')
parser.add_argument("--hash", help="An integer for the height to be queried", type=int)
args = parser.parse_args()

if not (args.hash):
    parser.error('No arguments provided.')

# $ curl -X POST http://127.0.0.1:18081/get_transactions -d '{"txs_hashes":["d6e48158472848e6687173a91ae6eebfa3e1d778e65252ee99d7515d63090408"]}' -H 'Content-Type: application/json'

answer = instruct_daemon('get_transactions', {"txs_hashes":[args.hash], "decode_as_json": True})
print(json.dumps(answer, indent=4, sort_keys=True))


