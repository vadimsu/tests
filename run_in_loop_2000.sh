while true
do
    ./tcp_test  1448 0 1000000000 1000 7777 7777 192.168.1.1 192.168.1.2 &
    ./tcp_test  1448 0 1000000000 1000 7778 7777 192.168.1.1 192.168.1.2 &
    sleep 30
    killall tcp_test
done
