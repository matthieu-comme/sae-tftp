# ================= VARIABLES =================
CC = gcc
# Flags: -Iinclude permet d'inclure les .h sans changer vos #include "..." dans le code
CFLAGS = -Wall -Wextra -g -Iinclude

# Nom de l'exécutable final
NAME = tftp
TEST_NAME = run_tests

# Dossiers
SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include
TEST_DIR = tests

# Liste des sources pour le programme principal
# On exclut test_unit.c qui est traité à part
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/client.c \
       $(SRC_DIR)/server.c \
       $(SRC_DIR)/sockets.c \
       $(SRC_DIR)/tftp_utils.c

# Transformation des .c en .o (ex: src/main.c -> obj/main.o)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# ================= REGLES =================

# Règle par défaut (lance la compil du programme principal)
all: $(NAME)

# Linkage de l'exécutable final
$(NAME): $(OBJS)
	@echo "Création de l'exécutable $(NAME)..."
	$(CC) $(CFLAGS) $(OBJS) -o $(NAME)
	@echo "Terminé !"

# Compilation des fichiers objets (.c -> .o)
# Le dossier obj est créé s'il n'existe pas
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Règle pour compiler et lancer les tests unitaires
# Note: test_unit a besoin de tftp_utils.o mais pas de main.o
tests: $(OBJ_DIR)/tftp_utils.o
	@echo "Compilation des tests..."
	$(CC) $(CFLAGS) $(TEST_DIR)/test_unit.c $(OBJ_DIR)/tftp_utils.o -o $(TEST_NAME)
	@echo "Lancement des tests :"
	@./$(TEST_NAME)

# Nettoyage des objets
clean:
	@echo "Suppression des objets..."
	rm -rf $(OBJ_DIR)

# Nettoyage complet (objets + exécutables)
fclean: clean
	@echo "Suppression des exécutables..."
	rm -f $(NAME) $(TEST_NAME)

# Recompiler depuis zéro
re: fclean all

.PHONY: all clean fclean re tests