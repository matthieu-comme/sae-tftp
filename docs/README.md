bind: Address already in use
sudo lsof -i :69 ## identifie le coupable ##
sudo systemctl stop tftpd-hpa

Lancer serveur :
sudo ./tftp server .

Côté client :
./tftp client get 127.0.0.1 file.txt out.txt
