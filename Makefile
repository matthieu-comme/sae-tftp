# variables
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude

CLIENT_NAME = tftp_client
SERVER_NAME = tftp_server
TEST_NAME = run_tests

# dossiers
SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include
TEST_DIR = tests

# sources communes (pas de main ici)
COMMON_SRCS = $(SRC_DIR)/sockets.c \
              $(SRC_DIR)/tftp_utils.c

# sources client/serveur (chacun contient SON main)
CLIENT_SRCS = $(SRC_DIR)/client.c
SERVER_SRCS = $(SRC_DIR)/server.c

COMMON_OBJS = $(COMMON_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(CLIENT_NAME) $(SERVER_NAME)

# ---------- client ----------
$(CLIENT_NAME): $(COMMON_OBJS) $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# ---------- serveur ----------
$(SERVER_NAME): $(COMMON_OBJS) $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# ---------- compilation objets ----------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- tests ----------
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
	rm -f $(CLIENT_NAME) $(SERVER_NAME) $(TEST_NAME)

re: fclean all

.PHONY: all clean fclean re tests
