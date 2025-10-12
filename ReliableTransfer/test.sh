make
gnome-terminal -e "server/udp_server 8080"
gnome-terminal -e "client/udp_client localhost 8080"
