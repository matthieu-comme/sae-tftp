1. Compilation

   Générer les exécutables : make

   Nettoyer le projet : make fclean

2. Lancement du Serveur

   Syntaxe : ./tftp_server <port> [dossier_racine]

   Exemple : ./tftp_server 6969 test_srv_dir

3. Utilisation du Client

   Options : -b (bigfile) et -w <taille> (windowsize)

   Télécharger un fichier (GET) : ./tftp_client [-b] [-w windowsize] get <ip> <port> <fichier_distant> <fichier_local>

   Envoyer un fichier (PUT) : ./tftp_client [-b] [-w windowsize] put <ip> <port> <fichier_local> <fichier_distant>

4. Tests

   Tests unitaires (C) : make tests (compile et exécute automatiquement les tests)

   Tests d'intégration (Python) : python3 tests/test_integration.py (compile le projet et lance les scénarios)
