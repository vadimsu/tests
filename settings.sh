sudo sysctl -w fs.file-max=500000
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65000"
sudo sysctl -w kernel.sem="250 256000 100 1024"
sudo sysctl -w net.core.rmem_max=40000000
sudo sysctl -w net.core.wmem_max=40000000
sudo sysctl -w net.core.rmem_default=20000000
sudo sysctl -w net.core.wmem_default=20000000
sudo sysctl -w net.core.optmem_max=40960
sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"
sudo sysctl -w net.core.netdev_max_backlog=100000
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=100000
sudo sysctl -w net.ipv4.tcp_max_tw_buckets=2000000
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.tcp_fin_timeout=10
sudo sysctl -w net.ipv4.tcp_slow_start_after_idle=0
sudo sysctl -w net.ipv4.tcp_tw_recycle=1

