# Nacent FRR with Openflow How-To

## get the code
```
git clone  https://github.com/LabNConsulting/frr-rfp-ovs-of.git
cd frr-rfp-ovs-of
git checkout -t remotes/origin/working/3.0/rfp-ovs-of+port-queues
```
## configure the code
This was tested on Ubuntu 18.04
```
./bootstrap.sh
automake bgpd/rfp-ovs-of/{librfp,rfptest}/Makefile
 ./configure --with-rfp-path=bgpd/rfp-ovs-of --enable-dev-build --enable-static --disable-shared --disable-vtysh --disable-zebra --disable-ripd --disable-ripngd --disable-ospfd --disable-ospf6d --disable-nhrpd --disable-watchfrr --disable-isisd --disable-pimd --disable-ospfapi --disable-ospfclient --disable-ldpd
```
## confirm that you have a git user set
```
git config --global --get-all user.email
```
if not set
```
git config --global  user.email <FILL_IN>
```

## build the code

Important: do not do parallel make at this 

```
make
```

Note that openflow is pulled from OVS (Apache licensed) and then automatically modified

## start bgpd

```
sudo mkdir -p /usr/local/var/run/openvswitch
sudo chown $USER /usr/local/var/run/openvswitch
bgpd/bgpd -f bgpd/rfp-ovs-of/sampleconfigs/2switches.conf -p 1234 -n --skip_runas -i pid.bgpd-openflow
```

## start mininet

Note: mininet needs to be installed before running 

Also: The package 'vlan' is required in Ubuntu or Debian, or 'vconfig' in Fedora

Change the ip=<IP_ADDRESS> if runinng bgp on a different host

```
cd bgpd/rfp-ovs-of/sampleconfigs
sudo mn --mac --switch ovsk,protocols=OpenFlow13 --controller remote,ip=127.0.0.1 --custom 2switches.py --topo mytopo
```

## test local traffic
```
mininet> l1 ping l2
PING 10.0.0.2 (10.0.0.2) 56(84) bytes of data.
64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=3.24 ms
64 bytes from 10.0.0.2: icmp_seq=2 ttl=64 time=0.122 ms
64 bytes from 10.0.0.2: icmp_seq=3 ttl=64 time=0.041 ms
^C
--- 10.0.0.2 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2000ms
rtt min/avg/max/mdev = 0.041/1.135/3.243/1.490 ms
mininet> l4 ping l5
PING 10.0.0.5 (10.0.0.5) 56(84) bytes of data.
64 bytes from 10.0.0.5: icmp_seq=1 ttl=64 time=1.91 ms
64 bytes from 10.0.0.5: icmp_seq=2 ttl=64 time=0.112 ms
^C
--- 10.0.0.5 ping statistics ---
2 packets transmitted, 2 received, 0% packet loss, time 1001ms
rtt min/avg/max/mdev = 0.112/1.014/1.917/0.903 ms
mininet> sh ovs-ofctl -O OpenFlow13 dump-flows s1
OFPST_FLOW reply (OF1.3) (xid=0x2):
 cookie=0x0, duration=4.055s, table=0, n_packets=1, n_bytes=98, idle_timeout=60, priority=1,in_port=4,dl_src=00:00:00:00:00:04,dl_dst=00:00:00:00:00:05 actions=output:5
 cookie=0x0, duration=4.055s, table=0, n_packets=2, n_bytes=196, idle_timeout=60, priority=1,in_port=5,dl_src=00:00:00:00:00:05,dl_dst=00:00:00:00:00:04 actions=output:4
 cookie=0x0, duration=16.286s, table=0, n_packets=3, n_bytes=246, idle_timeout=60, priority=1,in_port=1,dl_vlan=100,dl_src=00:00:00:00:00:01,dl_dst=00:00:00:00:00:02 actions=output:2
 cookie=0x0, duration=16.287s, table=0, n_packets=4, n_bytes=344, idle_timeout=60, priority=1,in_port=2,dl_vlan=100,dl_src=00:00:00:00:00:02,dl_dst=00:00:00:00:00:01 actions=output:1
 cookie=0x0, duration=421.652s, table=0, n_packets=6, n_bytes=376, priority=0 actions=CONTROLLER:65535
```

## test bridged traffic
```
mininet> l4 ping r4
PING 10.0.0.9 (10.0.0.9) 56(84) bytes of data.
64 bytes from 10.0.0.9: icmp_seq=1 ttl=64 time=2.87 ms
64 bytes from 10.0.0.9: icmp_seq=2 ttl=64 time=0.331 ms
64 bytes from 10.0.0.9: icmp_seq=3 ttl=64 time=0.067 ms
^C
--- 10.0.0.9 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2001ms
rtt min/avg/max/mdev = 0.067/1.090/2.872/1.264 ms
mininet> l1 ping r1
PING 10.0.0.6 (10.0.0.6) 56(84) bytes of data.
64 bytes from 10.0.0.6: icmp_seq=1 ttl=64 time=4.59 ms
64 bytes from 10.0.0.6: icmp_seq=2 ttl=64 time=0.363 ms
64 bytes from 10.0.0.6: icmp_seq=3 ttl=64 time=0.050 ms
^C
--- 10.0.0.6 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2000ms
rtt min/avg/max/mdev = 0.050/1.670/4.598/2.074 ms
mininet> sh ovs-ofctl -O OpenFlow13 dump-flows s1
OFPST_FLOW reply (OF1.3) (xid=0x2):
 cookie=0x0, duration=4.13s, table=0, n_packets=2, n_bytes=200, idle_timeout=60, priority=1,in_port=1,dl_vlan=100,dl_src=00:00:00:00:00:01,dl_dst=00:00:00:00:00:06 actions=output:100
 cookie=0x0, duration=4.131s, table=0, n_packets=3, n_bytes=302, idle_timeout=60, priority=1,in_port=100,dl_vlan=100,dl_src=00:00:00:00:00:06,dl_dst=00:00:00:00:00:01 actions=output:1
 cookie=0x0, duration=10.822s, table=0, n_packets=3, n_bytes=238, idle_timeout=60, priority=1,in_port=4,dl_src=00:00:00:00:00:04,dl_dst=00:00:00:00:00:09 actions=output:200
 cookie=0x0, duration=10.822s, table=0, n_packets=4, n_bytes=336, idle_timeout=60, priority=1,in_port=200,dl_src=00:00:00:00:00:09,dl_dst=00:00:00:00:00:04 actions=output:4
 cookie=0x0, duration=561.708s, table=0, n_packets=12, n_bytes=752, priority=0 actions=CONTROLLER:65535
mininet> sh ovs-ofctl -O OpenFlow13 dump-flows s2
OFPST_FLOW reply (OF1.3) (xid=0x2):
 cookie=0x0, duration=5.885s, table=0, n_packets=3, n_bytes=246, idle_timeout=60, priority=1,in_port=100,dl_vlan=100,dl_src=00:00:00:00:00:01,dl_dst=00:00:00:00:00:06 actions=output:1
 cookie=0x0, duration=5.886s, table=0, n_packets=4, n_bytes=344, idle_timeout=60, priority=1,in_port=1,dl_vlan=100,dl_src=00:00:00:00:00:06,dl_dst=00:00:00:00:00:01 actions=output:100
 cookie=0x0, duration=12.576s, table=0, n_packets=3, n_bytes=238, idle_timeout=60, priority=1,in_port=200,dl_src=00:00:00:00:00:04,dl_dst=00:00:00:00:00:09 actions=output:4
 cookie=0x0, duration=12.578s, table=0, n_packets=4, n_bytes=336, idle_timeout=60, priority=1,in_port=4,dl_src=00:00:00:00:00:09,dl_dst=00:00:00:00:00:04 actions=output:200
 cookie=0x0, duration=563.463s, table=0, n_packets=8, n_bytes=464, priority=0 actions=CONTROLLER:65535
```

## test label swap
```
mininet> l3 ping r3
PING 10.0.0.8 (10.0.0.8) 56(84) bytes of data.
64 bytes from 10.0.0.8: icmp_seq=1 ttl=64 time=2.88 ms
64 bytes from 10.0.0.8: icmp_seq=2 ttl=64 time=0.304 ms
64 bytes from 10.0.0.8: icmp_seq=3 ttl=64 time=0.154 ms
^C
--- 10.0.0.8 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2000ms
rtt min/avg/max/mdev = 0.154/1.114/2.885/1.253 ms
mininet> sh ovs-ofctl -O OpenFlow13 dump-flows s2
OFPST_FLOW reply (OF1.3) (xid=0x2):
 cookie=0x0, duration=4.414s, table=0, n_packets=3, n_bytes=298, idle_timeout=60, priority=1,in_port=3,dl_vlan=400,dl_src=00:00:00:00:00:08,dl_dst=00:00:00:00:00:03 actions=strip_vlan,push_vlan:0x8100,set_field:300->vlan_vid,output:200
 cookie=0x0, duration=4.412s, table=0, n_packets=2, n_bytes=200, idle_timeout=60, priority=1,in_port=200,dl_vlan=300,dl_src=00:00:00:00:00:03,dl_dst=00:00:00:00:00:08 actions=strip_vlan,push_vlan:0x8100,set_field:400->vlan_vid,output:3
 cookie=0x0, duration=4.414s, table=0, n_packets=0, n_bytes=0, idle_timeout=60, priority=1,in_port=200,dl_vlan=300,dl_src=00:00:00:00:00:03,dl_dst=ff:ff:ff:ff:ff:ff actions=strip_vlan,push_vlan:0x8100,set_field:400->vlan_vid,output:3
 cookie=0x0, duration=9602.706s, table=0, n_packets=11, n_bytes=658, priority=0 actions=CONTROLLER:65535
```

## Check running state on bgpd
```
telnet localhost 2605
Trying ::1...
Connected to localhost.
Escape character is '^]'.

Hello, this is FRRouting (version 3.0).
Copyright 1996-2005 Kunihiro Ishiguro, et al.


User Access Verification

Password: <zebra>
NVA-2SW> show vnc openflow switches 
Switch DPID:            0000000000000001        (FD=15  RFD=0x1ebd880)
VLANS:          Used
Flowsmode:      Wildcarded
Default group ports:    65534
L2 group ports: 4       Untagged    Group: sw1-null             RT: 0:1000
                5       Untagged    Group: sw1-null             RT: 0:1000
                200     Untagged    Group: sw1-null             RT: 0:1000
                1       VLAN: 100   Group: sw1-v100             RT: 0:1100
                2       VLAN: 100   Group: sw1-v100             RT: 0:1100
                100     VLAN: 100   Group: sw1-v100             RT: 0:1100
                3       VLAN: 300   Group: sw1-v300             RT: 0:1300
                200     VLAN: 300   Group: sw1-v300             RT: 0:1300

Switch DPID:            0000000000000002        (FD=14  RFD=0x1e86d70)
VLANS:          Used
Flowsmode:      Wildcarded
Default group ports:    65534
L2 group ports: 4       Untagged    Group: sw2-null             RT: 0:2000
                5       Untagged    Group: sw2-null             RT: 0:2000
                200     Untagged    Group: sw2-null             RT: 0:2000
                1       VLAN: 100   Group: sw2-v100             RT: 0:2100
                2       VLAN: 100   Group: sw2-v100             RT: 0:2100
                100     VLAN: 100   Group: sw2-v100             RT: 0:2100
                200     VLAN: 300   Group: sw2-v300             RT: 0:2400
                3       VLAN: 400   Group: sw2-v400             RT: 0:2400
```
## other useful commands
```
show vnc registrations

enable
clear openflow connections ...
clear openflow flows ...
```
