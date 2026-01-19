CC = gcc
CFLAGS = -Wall -Wextra

client: main.c tftp_utils.c tftp_utils.h
	$(CC) $(CFLAGS) main.c tftp_utils.c -o tftp_client

test_unit: test_unit.c tftp_utils.c tftp_utils.h
	$(CC) $(CFLAGS) test_unit.c tftp_utils.c -o test_unit

clean:
	rm -f tftp_client test_unit