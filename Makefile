# variables
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude

NAME = tftp
TEST_NAME = run_tests

# dossiers
SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include
TEST_DIR = tests

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/client.c \
       $(SRC_DIR)/server.c \
       $(SRC_DIR)/sockets.c \
       $(SRC_DIR)/tftp_utils.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(NAME)

$(NAME): $(OBJS)
	@echo "Création de l'exécutable $(NAME)..."
	$(CC) $(CFLAGS) $(OBJS) -o $(NAME)
	@echo "Terminé !"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

tests: $(OBJ_DIR)/tftp_utils.o
	@echo "Compilation des tests..."
	$(CC) $(CFLAGS) $(TEST_DIR)/test_unit.c $(OBJ_DIR)/tftp_utils.o -o $(TEST_NAME)
	@echo "Lancement des tests :"
	@./$(TEST_NAME)

clean:
	@echo "Suppression des objets..."
	rm -rf $(OBJ_DIR)

fclean: clean
	@echo "Suppression des exécutables..."
	rm -f $(NAME) $(TEST_NAME)

re: fclean all

.PHONY: all clean fclean re tests