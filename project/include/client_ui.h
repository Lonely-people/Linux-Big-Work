#ifndef PRACTICE_CLIENT_UI_H
#define PRACTICE_CLIENT_UI_H
/* Run the alternate-screen session browser/chat. 0: exit, -1: failure. */
int client_ui_run(int socket_fd);
int client_ui_available(void);
#endif
