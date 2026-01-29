# Le serveur doit être lancé avec les droits admin pour écouter sur le port 69

sudo ./tftp_server .

# Usage : ./tftp client get <ip_serveur> <fichier_distant> <fichier_local>

# télécharger le fichier file.txt et le nommer out.txt

./tftp client get 127.0.0.1 file.txt out.txt

# Usage : ./tftp client put <ip_serveur> <fichier_local> <fichier_distant>

# upload le fichier document.txt sous le nom backup.txt

./tftp client put 127.0.0.1 document.txt backup.txt

# compiler

make

# compiler et executer les tests

make tests

# supprimer les fichiers objets et les exécutables

make clean
