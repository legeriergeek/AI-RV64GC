sudo ip tuntap add dev tap0 mode tap user $USER
# Bring the interface up
sudo ip link set tap0 up
# (Optional, but required for internet access): 
# Assign an IP to the host side of the TAP interface
sudo ip addr add 192.168.100.1/24 dev tap0
# Enable IP forwarding and NAT so the VM can reach the internet
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -o enp6s0 -j MASQUERADE 