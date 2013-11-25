#include <libguile.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "mud_connection.h"
#include "mud_session.h"
#include "mud_ui.h"

static SCM write_to_stderr(SCM output)
{
    char *str = scm_to_locale_string(output);
    fprintf(stderr, "%s", str);
    free(str);

    return SCM_UNSPECIFIED;
}

static mud_session *main_session = NULL;
static SCM send_command(SCM command)
{
    char *str = scm_to_locale_string(command);
    int result = mud_connection_send_command(main_session->connection, str, strlen(str));
    free(str);

    return scm_from_int(result);
}

static SCM reload_config(void)
{
    scm_c_eval_string("(catch #t (lambda () (primitive-load config-filename)) (lambda (key . args) #t) (lambda (key . args) (display-backtrace (make-stack #t) (current-output-port))))");

    return SCM_UNSPECIFIED;
}

void init_guile(void)
{
    // Set up debugging.
    //SCM_DEVAL_P = 1;
    //SCM_BACKTRACE_P = 1;
    //SCM_RECORD_POSITIONS_P = 1;
    //SCM_RESET_DEBUG_MODE;

    // Set up the hooks.
    scm_c_define_gsubr("write-to-stderr", 1, 0, 0, &write_to_stderr);
    scm_c_define("send-command-hook", scm_make_hook(scm_from_int(1)));
    scm_c_define_gsubr("send-command", 1, 0, 0, &send_command);
    scm_c_define("config-filename", scm_from_locale_string("mud.scm"));
    scm_c_define_gsubr("reload-config", 0, 0, 0, &reload_config);

    // Load up regex support.
    scm_c_use_module("ice-9 regex");

    // Load the config file.
    reload_config();
    /*scm_c_eval_string("(define (lazy-catch-handler key . args)\
            (let ((stack (make-stack #t lazy-catch-handler)))\
             (display-backtrace stack (current-output-port))\
             (scm-error key \"?\" \"?\" args)\
             )\
                )\
              (catch #t (lambda () (lazy-catch #t\
                             (lambda () (primitive-load config-filename))\
                                           lazy-catch-handler))\
               (lambda (key . args) #t))");*/
    //scm_c_eval_string("(catch #t (lambda () (primitive-load config-filename)) (lambda (key . args) #t) (lambda (key . args) (display-backtrace (make-stack #t) (current-output-port))))");
    //scm_c_primitive_load("mud.scm");
}

void init_ncurses(void)
{
    initscr(); // Start ncurses mode.
    cbreak(); // Pass characters directly to the ui without waiting for the return key to be pressed.
    noecho(); // Don't echo characters as they are typed.
    start_color(); // Start colors.
    use_default_colors(); // Allows default colors (i.e. the usual color of the terminal foreground or background).
    init_color_pairs(); // Set up the color pairs to be used throughout.
}

void end_ncurses(void)
{
    endwin(); // End ncurses mode.
}

int RESIZE_OCCURRED = 0;
void signal_handler(int signum)
{
    if(signum == SIGWINCH)
        RESIZE_OCCURRED = 1;
}

void main_with_guile(void *data, int argc, char **argv)
{
    if(argc != 3 && data == NULL) {
        printf("Usage: mud <host> <port>\n");
        exit(1);
    }

    // SET UP SIGNAL HANDLERS.
    // Handle the window resize signal.
    struct sigaction action;
    memset(&action, 0x00, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGWINCH, &action, NULL);

    // Purposely ignore SIGPIPE. When socket errors occur, they should
    // be checked for and handled at the location they occur.
    signal(SIGPIPE, SIG_IGN);

    // OPEN THE SYSLOG.
    openlog("mud", 0x00, LOG_USER);

    // SET UP GUILE.
    init_guile();

    // SET UP THE SESSION.
    main_session = mud_session_create(
        mud_connection_create(REAL_SOCKET_OPS),
        line_buffer_create(10000), // Scrollback data.
        line_buffer_create(100)); // History data.

    // CONNECT TO THE MUD SERVER.
    const char *host = argv[1];
    const char *port = argv[2];
    bool connected = mud_connection_connect(main_session->connection, host, port);
    if(!connected) {
        printf("Error: Could not make a connection to '%s' on port '%s'.\n", host, port);
        exit(1);
    }

    // SET UP THE USER INTERFACE.
    // Start ncurses.
    init_ncurses();

    // Create the UI.
    mud_ui *ui = mud_ui_create(REAL_WINDOW_OPS, REAL_WINDOW_OPS, 0, 0, LINES, COLS);

    // MAIN LOOP.
    int input_keycode = 0x0;
    mud_char_t received_data[8192];
    while(input_keycode != 65) {
        // Handle a resize if necessary.
        if(RESIZE_OCCURRED) {
            // Restart ncurses.
            endwin();
            refresh();
            clear();

            // Resize the UI.
            mud_ui_resize(ui, LINES, COLS);

            // Adjust the scrollback to account for the new window size. This
            // is necessary when the window is expanded to avoid unnecessary
            // blank space when scrolled all the way back.
            if((line_buffer_num_lines(main_session->scrollback_data) - mud_session_get_scrollback_index(main_session) + 1) < mud_ui_get_output_window_max_lines(ui))
                mud_session_set_scrollback_index(
                    main_session,
                    line_buffer_num_lines(main_session->scrollback_data) - mud_ui_get_output_window_max_lines(ui) + 1,
                    mud_ui_get_output_window_max_lines(ui));

            // Update the UI.
            mud_ui_update(ui, main_session->scrollback_data, mud_session_get_scrollback_index(main_session), main_session->input_data);

            // We're done here.
            RESIZE_OCCURRED = 0;
        }

        // Check for incoming data from the server.
        int received = mud_connection_receive(main_session->connection, received_data, 24576);
        bool connected = mud_connection_connected(main_session->connection);
        if(received < 0 || !connected) {
            // TODO: Handle this case!
            break;
        }
        if(received > 0) {
            // Write the received data, keeping the scrollback locked, if necessary.
            // TODO: Fix this so that it works when the scrollback buffer is full.
            // Currently, in this case, no adjustment will be made to lock scrollback
            // in the right place. Maybe write could return the number
            // of lines written?
            int lines_before = line_buffer_num_lines(main_session->scrollback_data);
            line_buffer_write(main_session->scrollback_data, received_data, received);
            int lines_after = line_buffer_num_lines(main_session->scrollback_data);
            if(mud_session_get_scrollback_index(main_session) > 0)
                mud_session_adjust_scrollback_index(main_session, lines_after - lines_before, mud_ui_get_output_window_max_lines(ui));

            // UPDATE THE UI.
            mud_ui_update(ui, main_session->scrollback_data, mud_session_get_scrollback_index(main_session), main_session->input_data);
        }

        // Attempt to get a char from the user.
        input_keycode = mud_ui_get_char(ui);
        switch(input_keycode) {
            case ERR:
                usleep(10000);
                continue;
            case 0xA:
            {
                int lines_before = line_buffer_num_lines(main_session->scrollback_data);
                char *input = mud_string_to_c_str(main_session->input_data);
                line_buffer_write(main_session->history_data, main_session->input_data->_data, mud_string_length(main_session->input_data));
                mud_char_t newline = '\n';
                line_buffer_write(main_session->history_data, &newline, 1);
                mud_string_clear(main_session->input_data);
                int lines_after = line_buffer_num_lines(main_session->scrollback_data);

                // Keep scrollback locked for this line, if necessary.
                if(mud_session_get_scrollback_index(main_session) > 0)
                    mud_session_adjust_scrollback_index(main_session, lines_after - lines_before, mud_ui_get_output_window_max_lines(ui));

                // Run the hook.
                SCM symbol = scm_c_lookup("send-command-hook");
                SCM send_command_hook = scm_variable_ref(symbol);
                if(scm_is_true(scm_hook_p(send_command_hook)) && scm_is_false(scm_hook_empty_p(send_command_hook))) {
                    scm_c_run_hook(send_command_hook, scm_list_1(scm_from_locale_string(input)));
                } else {
                    // Send the command to the server.
                    int send_result = mud_connection_send_command(main_session->connection, input, strlen(input));
                    if(send_result < 0) {
                        // TODO: Handle this case!
                        free(input);
                        break;
                    }
                }

                free(input);
                break;
            }
            case 0xD:
                break;
            case KEY_RESIZE:
                break;
            case 27:
                mud_session_set_history_index(main_session, 0);
                mud_string_assign(main_session->input_data, line_buffer_get_line_relative_to_current(main_session->history_data, mud_session_get_history_index(main_session)));
                break;
            case KEY_UP:
                mud_session_adjust_history_index(main_session, 1);
                mud_string_assign(main_session->input_data, line_buffer_get_line_relative_to_current(main_session->history_data, mud_session_get_history_index(main_session)));
                break;
            case KEY_DOWN:
                mud_session_adjust_history_index(main_session, -1);
                mud_string_assign(main_session->input_data, line_buffer_get_line_relative_to_current(main_session->history_data, mud_session_get_history_index(main_session)));
                break;
            case KEY_PPAGE:
                //mud_ui_page_up(ui);
                mud_session_adjust_scrollback_index(main_session, 1, mud_ui_get_output_window_max_lines(ui));
                break;
            case KEY_NPAGE:
                //mud_ui_page_down(ui);
                mud_session_adjust_scrollback_index(main_session, -1, mud_ui_get_output_window_max_lines(ui));
                break;
            case KEY_BACKSPACE:
            case 127:
                mud_string_delete_char(main_session->input_data);
                break;
            default:
            {
                mud_char_t letter = input_keycode & 0xFF;
                mud_string_append(main_session->input_data, &letter, 1);
                break;
            }
        }

        // UPDATE THE UI.
        mud_ui_update(ui, main_session->scrollback_data, mud_session_get_scrollback_index(main_session), main_session->input_data);
    }

    end_ncurses();
    mud_session_destroy(main_session);
    mud_ui_destroy(ui);
}

int main(int argc, char **argv)
{
    scm_boot_guile(argc, argv, &main_with_guile, NULL);

    // Never reached.
    return 0;
}
