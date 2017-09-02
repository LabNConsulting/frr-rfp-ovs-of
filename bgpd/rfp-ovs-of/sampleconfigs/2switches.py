#!/usr/bin/python

#sudo mn --mac --switch ovsk,protocols=OpenFlow13 --controller remote,ip=192.168.122.1 --custom 2switches.py --topo mytopo

from mininet.node import Host
from mininet.topo import Topo

class VLANHost( Host ):
    "Host connected to VLAN interface"

    def config( self, vlan=100, **params ):
        """Configure VLANHost according to (optional) parameters:
           vlan: VLAN ID for default interface"""

        r = super( VLANHost, self ).config( **params )

        intf = self.defaultIntf()
        # remove IP from default, "physical" interface
        self.cmd( 'ifconfig %s inet 0' % intf )
        # create VLAN interface
        self.cmd( 'vconfig add %s %d' % ( intf, vlan ) )
        # assign the host's IP to the VLAN interface
        self.cmd( 'ifconfig %s.%d inet %s' % ( intf, vlan, params['ip'] ) )
        # update the intf name and host's intf map
        newName = '%s.%d' % ( intf, vlan )
        # update the (Mininet) interface to refer to VLAN interface name
        intf.name = newName
        # add VLAN interface to host's name to intf map
        self.nameToIntf[ newName ] = intf

        return r

class MyTopo( Topo ):
    "Simple topology example."
    def __init__( self ):
 
       "Create custom topo."
 
       # Initialize topology
       Topo.__init__( self )
 
       # Add hosts and switches
       leftHost1 = self.addHost( 'l1', cls=VLANHost, vlan=100 )
       leftHost2 = self.addHost( 'l2', cls=VLANHost, vlan=100 )
       leftHost3 = self.addHost( 'l3', cls=VLANHost, vlan=300 )
       leftHost4 = self.addHost( 'l4' )
       leftHost5 = self.addHost( 'l5' )

       rightHost1 = self.addHost( 'r1', cls=VLANHost, vlan=100 )
       rightHost2 = self.addHost( 'r2', cls=VLANHost, vlan=100 )
       rightHost3 = self.addHost( 'r3', cls=VLANHost, vlan=400 )
       rightHost4 = self.addHost( 'r4' )
       rightHost5 = self.addHost( 'r5' )

       leftSwitch = self.addSwitch( 's1')
       rightSwitch = self.addSwitch( 's2' )
 
       # Add links
       self.addLink( leftHost1, leftSwitch )
       self.addLink( leftHost2, leftSwitch )
       self.addLink( leftHost3, leftSwitch )
       self.addLink( leftHost4, leftSwitch )
       self.addLink( leftHost5, leftSwitch )

       self.addLink( rightHost1, rightSwitch )
       self.addLink( rightHost2, rightSwitch )
       self.addLink( rightHost3, rightSwitch )
       self.addLink( rightHost4, rightSwitch )
       self.addLink( rightHost5, rightSwitch )

       self.addLink( leftSwitch,  rightSwitch, 100, 100)
       self.addLink( leftSwitch,  rightSwitch, 200, 200)
 
topos = { 'mytopo': ( lambda: MyTopo() ) }
