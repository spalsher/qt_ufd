#!/usr/bin/env python3
"""Single-sender bench tool, not the production shared booth controller."""
import argparse, json, os, time
from urllib.request import Request, urlopen
from urllib.parse import urlencode
from urllib.error import HTTPError, URLError

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('state',choices=['waiting','checking','paid','rfid-paid','assist','idle','closed','open'])
p.add_argument('--passage',default='bench-1')
p.add_argument('--plate',default='AAA 000')
p.add_argument('--fare',default='200')
p.add_argument('--balance')
a=p.parse_args()
base=os.environ.get('UFD_URL','http://192.168.78.12').rstrip('/')
token=os.environ.get('UFD_TOKEN','')
if not token: p.error('set UFD_TOKEN to the token configured in UfdConfig.h')
headers={'Authorization':'Bearer '+token}
try:
    with urlopen(Request(base+'/v1/status',headers=headers),timeout=2) as r:
        status=json.load(r)
    if not status['ready']: raise RuntimeError('Panel driver is not ready')
    frame=dict(bootId=status['bootId'],sequence=status['sequence']+1,
               expiresAtMs=(status['uptimeMs']+5000)&0xffffffff,
               passageId=a.passage,state=a.state,plate=a.plate,fare=a.fare)
    if a.balance is not None: frame['balance']=a.balance
    request=Request(base+'/v1/frame?'+urlencode(frame),data=b'',headers=headers,method='POST')
    for attempt in range(2):
        try:
            with urlopen(request,timeout=2) as r: print(json.dumps(json.load(r),indent=2))
            break
        except HTTPError as e:
            raise RuntimeError(f'HTTP {e.code}: {e.read().decode()}') from e
        except (URLError,TimeoutError):
            if attempt: raise
            time.sleep(.1) # same sequence and payload; never reassign a failed old event
except (HTTPError,URLError,TimeoutError,RuntimeError) as e:
    p.exit(1,str(e)+'\n')
