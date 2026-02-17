# ================= tests_integration.py =================
import subprocess
import time
import os
import hashlib
import shutil

SERVER_PORT = 9069
SERVER_IP = "127.0.0.1"
ROOT_SRV = "test_srv_dir"
ROOT_CLI = "test_cli_dir"
SERVER_FILE = "./tftp_server"
CLIENT_FILE = "./tftp_client"

def setup():
    if os.path.exists(ROOT_SRV): shutil.rmtree(ROOT_SRV)
    if os.path.exists(ROOT_CLI): shutil.rmtree(ROOT_CLI)
    os.makedirs(ROOT_SRV)
    os.makedirs(ROOT_CLI)

def create_file(path, size_kb):
    with open(path, 'wb') as f:
        f.write(os.urandom(size_kb * 1024))

def md5(fname):
    hash_md5 = hashlib.md5()
    with open(fname, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            hash_md5.update(chunk)
    return hash_md5.hexdigest()

def run_test():
    print("--- Compilation ---")
    subprocess.check_call(["make"]) 
    
    setup()
    
    # 1. Lancement Serveur
    print(f"--- Lancement Serveur (Port {SERVER_PORT}) ---")
    srv_proc = subprocess.Popen([SERVER_FILE, str(SERVER_PORT), ROOT_SRV], 
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1) # Laisser le temps de bind

    try:
        # TEST 1: PUT (Upload petit fichier)
        print("[TEST 1] PUT small file")
        src = f"{ROOT_CLI}/upload.bin"
        dst = f"{ROOT_SRV}/upload.bin"
        create_file(src, 1) # 1KB
        
        subprocess.check_call([CLIENT_FILE, "put", SERVER_IP, str(SERVER_PORT), src, "upload.bin"])

        print(" -> Client fini. Attente écriture disque...")
        time.sleep(3) # On laisse 1 seconde au serveur pour flusher/fermer le fichier
        
        if not os.path.exists(dst): raise Exception("Fichier non reçu par serveur")
        
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
        print("[TEST 2] GET large file (5MB)")
        src = f"{ROOT_SRV}/download.bin"
        dst = f"{ROOT_CLI}/download.bin"
        create_file(src, 5120) # 5MB
        
        subprocess.check_call([CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "download.bin", dst])
        
        if not os.path.exists(dst): raise Exception("Fichier non reçu par client")
        if md5(src) != md5(dst): raise Exception("Contenu corrompu (MD5 mismatch)")
        print(" -> OK")

        # TEST 3: ERROR (Fichier inexistant)
        print("[TEST 3] GET missing file")
        ret = subprocess.call([CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "ghost.bin", "  .out"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ret == 0:
            print(" -> WARNING: Le client aurait dû retourner une erreur")
        else:
            print(" -> OK (Code retour erreur détecté)")


        # TEST 4: Fichier de taille multiple de 512 (ex: 1024 octets)
        print("[TEST 4] Edge Case: File size % 512 == 0")
        src = f"{ROOT_CLI}/boundary.bin"
        dst = f"{ROOT_SRV}/boundary.bin"
        create_file(src, 1) # 1KB exact (1024 octets)
        
        # Si le client gère mal, il attendra un dernier paquet qui ne vient jamais -> Timeout
        subprocess.check_call([CLIENT_FILE, "put", SERVER_IP, str(SERVER_PORT), src, "boundary.bin"])
        
        print(" -> Client fini. Attente écriture...")
        time.sleep(3)

        # DEBUG : Affiche les tailles pour comparer
        s_src = os.path.getsize(src)
        s_dst = os.path.getsize(dst)
        print(f" -> Taille Source: {s_src} octets | Taille Reçue: {s_dst} octets")
        
        if not os.path.exists(dst): raise Exception("Fichier boundary non reçu")
        if os.path.getsize(dst) != 1024: raise Exception(f"Taille incorrecte: {os.path.getsize(dst)}")
        if md5(src) != md5(dst): raise Exception("MD5 mismatch boundary")
        print(" -> OK")


        # TEST 5: Security - Directory Traversal
        print("[TEST 5] Security: Try to access ../Makefile")
        # On essaie de lire le Makefile qui est un cran au-dessus du dossier serveur
        dst = f"{ROOT_CLI}/hacked_makefile"
        
        # On s'attend à ce que le serveur refuse (Code retour != 0 ou fichier vide/erreur)
        ret = subprocess.call([CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "../Makefile", dst],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        # Vérifions que le serveur a envoyé une erreur (le client devrait retourner != 0)
        # OU BIEN que le client a créé un fichier contenant le message d'erreur TFTP
        if ret == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0:
            # Si on a réussi à télécharger le Makefile, c'est une FAIL
            with open(dst, 'rb') as f:
                content = f.read(10)
            if b"CC =" in content or b"gcc" in content: # contenu typique Makefile
                raise Exception("FAIL: Sécurité compromise, accès à ../ réussi !")
        
        print(" -> OK (Accès bloqué ou fichier non trouvé)")


        # TEST 6: Wrap Around Block Numbers (> 33MB)
        # 65536 blocs * 512 octets = 33 554 432 octets
        print("[TEST 6] Heavy Load: Block number wrap-around (>34MB)")
        src = f"{ROOT_SRV}/huge.bin"
        dst = f"{ROOT_CLI}/huge.bin"
        
        # Attention : création fichier un peu longue
        create_file(src, 50000) # 34MB (approx)
        
        start_t = time.time()
        subprocess.check_call([CLIENT_FILE, "get", SERVER_IP, str(SERVER_PORT), "huge.bin", dst])
        end_t = time.time()
        
        print(f" -> Transfert terminé en {end_t - start_t:.2f}s")
        if md5(src) != md5(dst): raise Exception("MD5 mismatch sur HUGE file")
        print(" -> OK")

    finally:
        srv_proc.terminate()
        srv_proc.wait()
        print("--- Fin des tests ---")

if __name__ == "__main__":
    run_test()