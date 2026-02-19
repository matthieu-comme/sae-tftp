# ================= tests_integration.py =================
# Auteur : Gemini
# Executer ce script compile les fichiers nécessaires et lance automatiquement les tests

import subprocess
import time
import os
import hashlib
import shutil
import socket

SERVER_PORT = 9069
SERVER_IP = "127.0.0.1"
ROOT_SRV = "test_srv_dir"
ROOT_CLI = "test_cli_dir"
SERVER_FILE = "./tftp_server_mono"
CLIENT_FILE = "./tftp_client"


def setup():
    if os.path.exists(ROOT_SRV):
        shutil.rmtree(ROOT_SRV)
    if os.path.exists(ROOT_CLI):
        shutil.rmtree(ROOT_CLI)
    os.makedirs(ROOT_SRV)
    os.makedirs(ROOT_CLI)


def create_file(path, size_kb):
    with open(path, "wb") as f:
        f.write(os.urandom(size_kb * 1024))


def md5(fname):
    hash_md5 = hashlib.md5()
    with open(fname, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            hash_md5.update(chunk)
    return hash_md5.hexdigest()


def test_error_illegal_opcode():
    print("[TEST ERROR 4] Illegal Opcode")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2)

    # Paquet malformé : Opcode 99 (inconnu)
    packet = b"\x00\x63" + b"test.bin\x00" + b"octet\x00"
    sock.sendto(packet, (SERVER_IP, SERVER_PORT))

    try:
        data, _ = sock.recvfrom(516)
        opcode = int.from_bytes(data[:2], "big")
        err_code = int.from_bytes(data[2:4], "big")

        if opcode == 5 and err_code == 4:
            print(" -> OK (Erreur 4 reçue)")
        else:
            raise Exception(f"Attendu Erreur 4, reçu Opcode {opcode} Code {err_code}")
    except socket.timeout:
        raise Exception("Le serveur n'a pas répondu à l'opcode invalide")
    finally:
        sock.close()


def test_error_unknown_tid():
    print("[TEST ERROR 5] Unknown TID (Intrusion)")

    # 1. On initialise une session normale (WRQ) avec un premier socket
    sock_legit = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_legit.settimeout(2)
    wrq = b"\x00\x02" + b"ghost.bin\x00" + b"octet\x00"
    sock_legit.sendto(wrq, (SERVER_IP, SERVER_PORT))

    # On récupère le port de session (TID) choisi par le serveur
    ack0, tid_addr = sock_legit.recvfrom(516)

    # 2. Un DEUXIÈME socket (l'intrus) essaie d'envoyer un DATA 1 au même TID
    sock_intruder = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_intruder.settimeout(2)
    data1 = b"\x00\x03\x00\x01" + b"hello"
    sock_intruder.sendto(data1, tid_addr)

    try:
        resp, _ = sock_intruder.recvfrom(516)
        opcode = int.from_bytes(resp[:2], "big")
        err_code = int.from_bytes(resp[2:4], "big")

        if opcode == 5 and err_code == 5:
            print(" -> OK (Erreur 5 envoyée à l'intrus)")
        else:
            raise Exception(f"L'intrus aurait dû recevoir Erreur 5, reçu {err_code}")
    except socket.timeout:
        raise Exception("Le serveur n'a pas rejeté l'intrus (Timeout)")
    finally:
        sock_legit.close()
        sock_intruder.close()


def test_error_access_violation_read():
    print("[TEST ERROR 2] Access Violation (Read)")
    path = f"{ROOT_SRV}/secret.bin"
    create_file(path, 1)
    os.chmod(path, 0o000)  # On retire tous les droits

    # Le client doit recevoir une erreur 2 lors du GET
    ret = subprocess.call(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "secret.bin", "out.bin"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    os.chmod(path, 0o644)  # Remise des droits pour le nettoyage
    if ret != 0:
        print(" -> OK (Le serveur a refusé la lecture du fichier protégé)")
    else:
        raise Exception("Le serveur aurait dû échouer sur le fichier sans droits")


def test_error_disk_full_simulated():
    print("[TEST ERROR 3] Disk Full / Write Error")
    # On crée un sous-dossier protégé dans le serveur
    protected_dir = f"{ROOT_SRV}/readonly_dir"
    if not os.path.exists(protected_dir):
        os.makedirs(protected_dir)
    os.chmod(protected_dir, 0o555)  # Lecture/Exécution seulement (pas d'écriture)

    src = f"{ROOT_CLI}/data.bin"
    create_file(src, 1)

    # Tentative de PUT dans le dossier protégé
    ret = subprocess.call(
        [CLIENT_FILE, "put", SERVER_IP, str(SERVER_PORT), src, "readonly_dir/data.bin"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    if ret != 0:
        print(" -> OK (Erreur d'écriture détectée)")
    else:
        raise Exception("Le serveur a réussi à écrire dans un répertoire protégé !")


# TEST 1: PUT (Upload petit fichier)
def test_put_small_file():
    print("[TEST 1] PUT small file")
    src = f"{ROOT_CLI}/upload.bin"
    dst = f"{ROOT_SRV}/upload.bin"
    create_file(src, 1)  # 1KB

    subprocess.check_call(
        [CLIENT_FILE, "put", SERVER_IP, str(SERVER_PORT), src, "upload.bin"]
    )

    print(" -> Client fini. Attente écriture disque...")
    time.sleep(3)  # On laisse 3 secondes au serveur pour flusher/fermer le fichier

    if not os.path.exists(dst):
        raise Exception("Fichier non reçu par serveur")

    # DEBUG : Affiche les tailles pour comparer
    s_src = os.path.getsize(src)
    s_dst = os.path.getsize(dst)
    print(f" -> Taille Source: {s_src} octets | Taille Reçue: {s_dst} octets")

    if s_src != s_dst:
        raise Exception(f"Tailles différentes ! ({s_src} vs {s_dst})")

    if md5(src) != md5(dst):
        raise Exception("Contenu corrompu (MD5 mismatch)")
    print(" -> OK")


# TEST 2: GET (Download gros fichier > 1 block)
def test_get_large_file():
    print("[TEST 2] GET large file (5MB)")
    src = f"{ROOT_SRV}/download.bin"
    dst = f"{ROOT_CLI}/download.bin"
    create_file(src, 5120)  # 5MB

    subprocess.check_call(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "download.bin", dst]
    )

    if not os.path.exists(dst):
        raise Exception("Fichier non reçu par client")
    if md5(src) != md5(dst):
        raise Exception("Contenu corrompu (MD5 mismatch)")
    print(" -> OK")


# TEST 3: ERROR (Fichier inexistant)
def test_error_missing_file():
    print("[TEST 3] GET missing file")
    ret = subprocess.call(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "ghost.bin", "  .out"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ret == 0:
        print(" -> WARNING: Le client aurait dû retourner une erreur")
    else:
        print(" -> OK (Code retour erreur détecté)")


# TEST 4: Fichier de taille multiple de 512 (ex: 1024 octets)
def test_multiple_512():
    print("[TEST 4] Edge Case: File size % 512 == 0")
    src = f"{ROOT_CLI}/boundary.bin"
    dst = f"{ROOT_SRV}/boundary.bin"
    create_file(src, 1)  # 1KB exact (1024 octets)

    # Si le client gère mal, il attendra un dernier paquet qui ne vient jamais -> Timeout
    subprocess.check_call(
        [CLIENT_FILE, "put", SERVER_IP, str(SERVER_PORT), src, "boundary.bin"]
    )

    print(" -> Client fini. Attente écriture...")
    time.sleep(3)

    # DEBUG : Affiche les tailles pour comparer
    s_src = os.path.getsize(src)
    s_dst = os.path.getsize(dst)
    print(f" -> Taille Source: {s_src} octets | Taille Reçue: {s_dst} octets")

    if not os.path.exists(dst):
        raise Exception("Fichier boundary non reçu")
    if os.path.getsize(dst) != 1024:
        raise Exception(f"Taille incorrecte: {os.path.getsize(dst)}")
    if md5(src) != md5(dst):
        raise Exception("MD5 mismatch boundary")
    print(" -> OK")


# TEST 5: Security - Directory Traversal
def test_security_access():
    print("[TEST 5] Security: Try to access ../Makefile")
    # On essaie de lire le Makefile qui est un cran au-dessus du dossier serveur
    dst = f"{ROOT_CLI}/hacked_makefile"

    # On s'attend à ce que le serveur refuse (Code retour != 0 ou fichier vide/erreur)
    ret = subprocess.call(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "../Makefile", dst],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Vérifions que le serveur a envoyé une erreur (le client devrait retourner != 0)
    # OU BIEN que le client a créé un fichier contenant le message d'erreur TFTP
    if ret == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0:
        # Si on a réussi à télécharger le Makefile, c'est une FAIL
        with open(dst, "rb") as f:
            content = f.read(10)
        if b"CC =" in content or b"gcc" in content:  # contenu typique Makefile
            raise Exception("FAIL: Sécurité compromise, accès à ../ réussi !")

    print(" -> OK (Accès bloqué ou fichier non trouvé)")


# TEST 6: Wrap Around Block Numbers (> 33MB)
def test_higher_block_number():
    # 65536 blocs * 512 octets = 33 554 432 octets
    print("[TEST 6] Heavy Load: Block number wrap-around (>34MB)")
    src = f"{ROOT_SRV}/huge.bin"
    dst = f"{ROOT_CLI}/huge.bin"

    # Attention : création fichier un peu longue
    create_file(src, 50000)

    start_t = time.time()
    subprocess.check_call(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "huge.bin", dst]
    )
    end_t = time.time()

    print(f" -> Transfert terminé en {end_t - start_t:.2f}s")
    if md5(src) != md5(dst):
        raise Exception("MD5 mismatch sur HUGE file")
    print(" -> OK")


# TEST 7: Concurrence put
def test_concurrency_put():
    print("\n[TEST 7] CONCURRENCY: 3 Clients simultanés")

    # 1. Préparation des fichiers
    # Un "Gros" fichier pour occuper le serveur (ex: 20 Mo)
    # Deux "Petits" fichiers pour voir s'ils passent pendant que le gros tourne
    src_big = f"{ROOT_CLI}/big_concurrent.bin"
    dst_big = f"{ROOT_SRV}/big_concurrent.bin"
    create_file(src_big, 20480)  # 20MB

    src_small_1 = f"{ROOT_CLI}/small_1.bin"
    dst_small_1 = f"{ROOT_SRV}/small_1.bin"
    create_file(src_small_1, 1)  # 1KB

    src_small_2 = f"{ROOT_CLI}/small_2.bin"
    dst_small_2 = f"{ROOT_SRV}/small_2.bin"
    create_file(src_small_2, 512)  # 512KB

    print(" -> Lancement des 3 processus clients en parallèle...")
    start_time = time.time()

    # On utilise Popen au lieu de check_call pour ne pas bloquer le script python
    # Client 1 : Upload GROS fichier
    p1 = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_big,
            "big_concurrent.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # Petite pause pour être sûr que P1 a commencé et occupe le serveur
    time.sleep(0.2)

    # Client 2 : Upload PETIT fichier
    p2 = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_small_1,
            "small_1.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # Client 3 : Upload MOYEN fichier
    p3 = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_small_2,
            "small_2.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # On attend que tout le monde finisse
    exit1 = p1.wait()
    exit2 = p2.wait()
    exit3 = p3.wait()

    end_time = time.time()
    print(f" -> Tous les clients ont fini en {end_time - start_time:.2f} secondes.")

    # Vérification des codes de retour
    if exit1 != 0:
        print(f"STDERR Client 1: {p1.stderr.read().decode()}")
        raise Exception("Client 1 (Gros) a échoué")
    if exit2 != 0:
        print(f"STDERR Client 2: {p2.stderr.read().decode()}")
        raise Exception("Client 2 (Petit) a échoué")
    if exit3 != 0:
        print(f"STDERR Client 3: {p3.stderr.read().decode()}")
        raise Exception("Client 3 (Moyen) a échoué")

    # Attente écriture disque serveur (sécurité)
    time.sleep(3)

    # Vérification MD5
    sz_src = os.path.getsize(src_big)
    sz_dst = os.path.getsize(dst_big)

    print(f" -> Client 1 (Gros) : Source {sz_src} octets | Reçu {sz_dst} octets")

    if sz_src != sz_dst:
        raise Exception(f"Erreur Taille Client 1 ! ({sz_src} vs {sz_dst})")
    if md5(src_big) != md5(dst_big):
        raise Exception(
            f"Corruption données (Client 1). Taille: {os.path.getsize(dst_big)}"
        )
    sz_src = os.path.getsize(src_small_1)
    sz_dst = os.path.getsize(dst_small_1)
    print(f" -> Client 2 (Petit) : Source {sz_src} octets | Reçu {sz_dst} octets")

    if sz_src != sz_dst:
        raise Exception(f"Erreur Taille Client 2 ! ({sz_src} vs {sz_dst})")

    if md5(src_small_1) != md5(dst_small_1):
        raise Exception("Corruption données (Client 2)")

    sz_src = os.path.getsize(src_small_2)
    sz_dst = os.path.getsize(dst_small_2)

    print(f" -> Client 3 (Moyen) : Source {sz_src} octets | Reçu {sz_dst} octets")

    if sz_src != sz_dst:
        raise Exception(f"Erreur Taille Client 3 ! ({sz_src} vs {sz_dst})")
    if md5(src_small_2) != md5(dst_small_2):
        raise Exception("Corruption données (Client 3)")

    print(" -> OK : Le serveur a géré 3 transferts simultanés !")


# TEST 8: Concurrence get
def test_concurrency_get():
    print("\n[TEST 8] CONCURRENCY GET: 3 Clients simultanés")

    # Fichiers sources (sur le serveur)
    src_big = f"{ROOT_SRV}/big_get.bin"
    src_small_1 = f"{ROOT_SRV}/small_get_1.bin"
    src_small_2 = f"{ROOT_SRV}/small_get_2.bin"

    # Fichiers de destination (téléchargés par les clients)
    dst_big = f"{ROOT_CLI}/big_get.bin"
    dst_small_1 = f"{ROOT_CLI}/small_get_1.bin"
    dst_small_2 = f"{ROOT_CLI}/small_get_2.bin"

    # Création des fichiers sur le serveur avant le test
    create_file(src_big, 20480)  # 20 Mo
    create_file(src_small_1, 1)  # 1 Ko
    create_file(src_small_2, 2)  # 2 Ko

    print(" -> Lancement des 3 processus clients en parallèle (GET)...")
    start_time = time.time()

    # Lancement des clients en mode GET
    # Syntaxe client : ./tftp_client get <server_ip> <port> <remote_file> <local_file>
    p1 = subprocess.Popen(
        [CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "big_get.bin", dst_big]
    )
    p2 = subprocess.Popen(
        [
            CLIENT_FILE,
            "get",
            SERVER_IP,
            str(SERVER_PORT),
            "small_get_1.bin",
            dst_small_1,
        ]
    )
    p3 = subprocess.Popen(
        [
            CLIENT_FILE,
            "get",
            SERVER_IP,
            str(SERVER_PORT),
            "small_get_2.bin",
            dst_small_2,
        ]
    )

    # Attente de la fin des 3 processus
    ret1 = p1.wait()
    ret2 = p2.wait()
    ret3 = p3.wait()

    elapsed = time.time() - start_time
    print(f" -> Tous les clients ont fini en {elapsed:.2f} secondes.")

    # Vérification des codes de retour
    if ret1 != 0:
        raise Exception("Client 1 (Gros) a échoué")
    if ret2 != 0:
        raise Exception("Client 2 (Petit 1) a échoué")
    if ret3 != 0:
        raise Exception("Client 3 (Petit 2) a échoué")

    # Vérification des tailles et des hash MD5
    sz_src_big = os.path.getsize(src_big)
    sz_dst_big = os.path.getsize(dst_big)
    print(
        f" -> Client 1 (Gros)  : Source {sz_src_big} octets | Reçu {sz_dst_big} octets"
    )
    if md5(src_big) != md5(dst_big):
        raise Exception("Corruption données (Client 1 - GET)")

    sz_src_s1 = os.path.getsize(src_small_1)
    sz_dst_s1 = os.path.getsize(dst_small_1)
    print(f" -> Client 2 (Petit) : Source {sz_src_s1} octets | Reçu {sz_dst_s1} octets")
    if md5(src_small_1) != md5(dst_small_1):
        raise Exception("Corruption données (Client 2 - GET)")

    sz_src_s2 = os.path.getsize(src_small_2)
    sz_dst_s2 = os.path.getsize(dst_small_2)
    print(f" -> Client 3 (Moyen) : Source {sz_src_s2} octets | Reçu {sz_dst_s2} octets")
    if md5(src_small_2) != md5(dst_small_2):
        raise Exception("Corruption données (Client 3 - GET)")

    print(" -> OK : Le serveur a géré 3 téléchargements (GET) simultanés !")


# TEST 9: Concurrence Lecteurs/Ecrivains (is_access_denied)
def test_concurrency_rw_lock():
    print("\n[TEST 9] CONCURRENCY R/W LOCK: is_access_denied")

    # --- SCENARIO A : 1 WRQ en cours bloque les autres RRQ et WRQ ---
    print(" -> Scénario A : WRQ actif bloque les autres requêtes...")
    src_big_put = f"{ROOT_CLI}/shared_put.bin"
    create_file(
        src_big_put, 102400
    )  # 20 Mo, assez long pour que les autres requêtes arrivent pendant le transfert

    # Lancement du WRQ (PUT) principal
    p_put_main = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_big_put,
            "shared_put.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    time.sleep(0.5)  # Laisse le temps au serveur d'ouvrir le fichier et poser le verrou

    # Tentative d'un GET (RRQ) concurrent sur le même fichier -> DOIT ÉCHOUER
    dst_get_blocked = f"{ROOT_CLI}/blocked_get.bin"
    p_get_blocked = subprocess.Popen(
        [
            CLIENT_FILE,
            "get",
            SERVER_IP,
            str(SERVER_PORT),
            "shared_put.bin",
            dst_get_blocked,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # Tentative d'un PUT (WRQ) concurrent sur le même fichier -> DOIT ÉCHOUER
    src_small_put = f"{ROOT_CLI}/small_put.bin"
    create_file(src_small_put, 1)
    p_put_blocked = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_small_put,
            "shared_put.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # Vérification des échecs (le client TFTP doit retourner un code d'erreur != 0 suite au paquet ERROR du serveur)
    if p_get_blocked.wait() == 0:
        raise Exception(
            "Échec Scénario A : Le GET concurrent a réussi alors qu'un PUT était en cours !"
        )
    if p_put_blocked.wait() == 0:
        raise Exception(
            "Échec Scénario A : Le 2eme PUT concurrent a réussi alors qu'un PUT était en cours !"
        )

    # Vérification de la réussite du PUT principal
    if p_put_main.wait() != 0:
        raise Exception("Le PUT principal a échoué.")
    print("    Scénario A validé !")

    time.sleep(5)
    # --- SCENARIO B : 1 RRQ en cours bloque les WRQ mais autorise les autres RRQ ---
    print(
        " -> Scénario B : RRQ actif bloque WRQ mais autorise les lecteurs multiples..."
    )
    src_big_get = f"{ROOT_SRV}/shared_get.bin"
    create_file(src_big_get, 307200)  # 300 Mo

    # Lancement du RRQ (GET) principal
    dst_get_main = f"{ROOT_CLI}/shared_get_main.bin"
    p_get_main = subprocess.Popen(
        [
            CLIENT_FILE,
            "get",
            SERVER_IP,
            str(SERVER_PORT),
            "shared_get.bin",
            dst_get_main,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    time.sleep(0.5)

    # Tentative d'un PUT (WRQ) concurrent -> DOIT ÉCHOUER
    p_put_blocked_b = subprocess.Popen(
        [
            CLIENT_FILE,
            "put",
            SERVER_IP,
            str(SERVER_PORT),
            src_small_put,
            "shared_get.bin",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    # Tentative d'un 2eme GET (RRQ) concurrent -> DOIT RÉUSSIR (Lecteurs multiples autorisés)
    dst_get_allowed = f"{ROOT_CLI}/shared_get_allowed.bin"
    p_get_allowed = subprocess.Popen(
        [
            CLIENT_FILE,
            "get",
            SERVER_IP,
            str(SERVER_PORT),
            "shared_get.bin",
            dst_get_allowed,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    # Vérifications
    if p_put_blocked_b.wait() == 0:
        raise Exception(
            "Échec Scénario B : Le PUT a réussi alors qu'un GET était en cours !"
        )

    if p_get_allowed.wait() != 0:
        raise Exception(
            "Échec Scénario B : Le 2eme GET a échoué (les lecteurs concurrents auraient dû être autorisés) !"
        )

    if p_get_main.wait() != 0:
        raise Exception("Le GET principal a échoué.")

    print("    Scénario B validé !")
    print(" -> OK : is_access_denied fonctionne correctement !")


def run_test():
    print("--- Compilation ---")
    subprocess.check_call(["make"])

    setup()

    # 1. Lancement Serveur
    print(f"--- Lancement Serveur (Port {SERVER_PORT}) ---")
    srv_proc = subprocess.Popen(
        [SERVER_FILE, str(SERVER_PORT), ROOT_SRV],
        # stdout=subprocess.DEVNULL,
        # stderr=subprocess.DEVNULL,
    )
    time.sleep(1)  # Laisser le temps de bind

    try:
        test_error_illegal_opcode()
        test_error_unknown_tid()
        test_error_access_violation_read()
        test_error_disk_full_simulated()

        test_put_small_file()
        test_get_large_file()
        test_error_missing_file()
        test_multiple_512()
        test_security_access()
        test_higher_block_number()
        test_concurrency_get()
        test_concurrency_put()
        test_concurrency_rw_lock()

    finally:
        srv_proc.terminate()
        srv_proc.wait()
        print("--- Fin des tests ---")


if __name__ == "__main__":
    run_test()
