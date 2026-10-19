#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <resolv.h>

#define MAX_FIELD_LEN 256
#define MAX_EXEC_ARGS 16

#define TYPE_SVCB 64 // DNS query type for SVCB records
#define TYPE_LEN 2 // bytes used by record type
#define CLASS_LEN 2 // bytes used by record class
#define TTL_LEN 4 // bytes used by TTL field
#define PRI_LEN 2 // bytes used by priority field
#define RDATA_LEN_LEN 2 // bytes used by rdata length field
#define PARAMETER_KEY_LEN 2 // bytes used by parameter key field
#define PARAMETER_LEN_LEN 2 // bytes used by parameter len field

#define ALPN_KEY 1
#define PORT_KEY 3

#define QUESTION_CNT_OFFSET 4 // absolute location of question count
#define ANSWER_CNT_OFFSET 6 // absolute location of answer count
#define QUESTIONS_OFFSET 12 // absolute start of question section

typedef struct {
    char host[MAX_FIELD_LEN];
    char alpn[MAX_FIELD_LEN];
    uint16_t port;
} svc_target_t;

/* Resolve the query name a SVCB record if present. Write the response into
 * the buffer supplied by the caller and return the length. */
int perform_dns_lookup(const char *query_name,
                       unsigned char *response, int max_response_len) {
    /* This is the only function where we use the resolver so we can
     * initialize and close it here and use the stack for vars. */
    struct __res_state state;
    memset(&state, 0, sizeof(state));

    if (res_ninit(&state) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize resolver.\n");
        return -1;
    }

    /* There are other ways to resolve SVCB names but this one honors the
     * suffix search order so use it. */
    int response_len = res_nsearch(&state, query_name, C_IN, TYPE_SVCB,
                                   response, max_response_len);
    if (response_len < 0) {
        fprintf(stderr, "[ERROR] DNS query failed for '%s'\n", query_name);
        return -1;
    }
    /* It is typical to call ns_initparse after res_nsearch but I don't
     * see the point for SVCB records since we have to parse the raw bytes
     * by hand anyways. */

    res_nclose(&state); // no return value, safe without a check
    return response_len;
}

/* Given a pointer to the DNS response and the number of questions, return
 * a pointer to the start of the answers immediately after the questions. */
const unsigned char *skip_questions(const unsigned char *r,
                                    int r_len,
                                    int q_cnt) {
    const unsigned char *ptr = r + QUESTIONS_OFFSET;
    char name[MAX_FIELD_LEN];

    /* Iterate over each question. Expand the domain name if needed, and
     * advance past it along with the type and class fields. */
    for (int i = 0; i < q_cnt; i++) {
        int name_len = dn_expand(r, r + r_len, ptr, name, sizeof(name));
        if(name_len< 0) return NULL;
        ptr += name_len + TYPE_LEN + CLASS_LEN;
    }
    return ptr;
}

/* Iterate over all answers looking for SVCB records. When one is found, stash
 * the values from it and return. */
int parse_svcb_record(const unsigned char *r,
                      int r_len,
                      const unsigned char *ptr,
                      int a_cnt,
                      svc_target_t *t) {
    char name[MAX_FIELD_LEN];

    for (int i = 0; i < a_cnt; i++) { // Loop over answers until SVCB found
        /* Attain the name and most past past it. I don't actually use this
         * name to do anything but dn_expand handles name compression and
         * tells how far to advance past it which is what we need. */
        int name_len = dn_expand(r, r + r_len, ptr, name, sizeof(name));
        if(name_len < 0) return -1;
        ptr += name_len;
        
        /* The cursor now points at the start of the response type field.
         * Grab and advance past along with the CLASS and TTL. */
        uint16_t rtype = ntohs(*(uint16_t *)ptr);
        ptr += TYPE_LEN + CLASS_LEN + TTL_LEN;

        /* Similar for the rdata length. This field is used to tell when we
         * have processed all of service parameters. */
        uint16_t rdata_len = ntohs(*(uint16_t *)ptr);
        ptr += RDATA_LEN_LEN;
        const unsigned char *rdata_end = ptr + rdata_len;

        if (rtype == TYPE_SVCB) {
            ptr += PRI_LEN;
            
            /* Expand and stash the hostname to connect to before advancing
             * the reply pointer. */
            name_len = dn_expand(r, r + r_len, ptr, t->host, sizeof(t->host));
            if(name_len < 0) return -1;
            ptr += name_len;

            /* Process the parameters within the rdata - in particular: ALPN
             * which represents the protocol to use and what port on the host
             * to connect to. */
            while (ptr < rdata_end) {
                /* Parse parameter key and length and advance the cursor. */
                uint16_t p_key = ntohs(*(uint16_t *)ptr);
                ptr += PARAMETER_KEY_LEN;
                uint16_t p_len = ntohs(*(uint16_t *)ptr);
                ptr += PARAMETER_LEN_LEN;

                /* Process ALPN and port parameters but no others. Advance
                 * the cursor past the parameter whether we use it or not. */
                if (p_key == ALPN_KEY && p_len > 0) {
                    /* This is a single byte hence no special cast is necessary.
                     * Add an extra byte for null terminator. Note that ALPN
                     * within DNS packet is not null-terminated*/
                    unsigned int alpn_str_len = *ptr;
                    if (alpn_str_len < sizeof(t->alpn)) {
                        memcpy(t->alpn, ptr + 1, alpn_str_len);
                        t->alpn[alpn_str_len] = '\0';
                    }
                } else if (p_key == PORT_KEY) {
                    t->port = ntohs(*(uint16_t *)ptr);
                } // else anything else is an unused parameter
                ptr += p_len;
            }
            /* This is the expected return point from this function. */
            return 0;
        } else {
            /* I'm not sure what other response type than SVCB would ever be
             * encountered but if it does happen, log it and skip over it. */
            fprintf(stderr, "[DEBUG] Unexpected response type: %d", rtype);
            ptr += rdata_len;
        }
    }
    /* Hopefully things don't get here. */
    return -1;
}

/* Some commands like ipmitool and openssh don't prompt for a username and
 * instead require one to be passed as a command line argument (or implied)
 * via environment. This function will try to get the username from the
 * environment but if that fails, asks for it interactively. */
void get_username(char *username, size_t max_len) {
    const char *env_user = getenv("USER");
    if (env_user && strlen(env_user) > 0) {
        strlcpy(username, env_user, max_len);
        fprintf(stderr, "[DEBUG] Using username from environment ($USER): '%s'\n", username);
        return;
    }

    /* prompt and then read a value. */
    fprintf(stdout, "Enter username: ");
    fflush(stdout);
    if (fgets(username, max_len, stdin) != NULL) {
        /* Strip newline and explicitly null terminate */
        username[strcspn(username, "\n")] = '\0';
    }
}

/* Determine what binary to execute depending on what value we got from DNS.
 * Build a set of command-specific arguments to supply it with.
 * Execute the program by replacing this program with it. */
void execute_binary(const svc_target_t *target) {
    char *bin; // The command to run, absolute or within $PATH
    char port_str[6]; // 16bit int will occupy 6 bytes w/NULL as a string
    snprintf(port_str, sizeof(port_str), "%u", target->port);

    char username[MAX_FIELD_LEN] = {0};
    char *exec_args[MAX_EXEC_ARGS];
    int arg_idx = 0;
    
    /* Build array of char* to pass to execvp. Handles ssh, ipmitool, and
     * telnet as methods to communicate with a console over the network.
     * Extend this section to add support for other methods. If an unexpected
     * value is encountered here, bail. */
    if (strcmp(target->alpn, "ssh") == 0) {
        bin = "ssh";
        exec_args[arg_idx++] = bin;
        get_username(username, sizeof(username));
        
        exec_args[arg_idx++] = "-l";
        exec_args[arg_idx++] = username;
        exec_args[arg_idx++] = "-p";
        exec_args[arg_idx++] = port_str;
        exec_args[arg_idx++] = (char *)target->host;

        fprintf(stdout, "Connecting over SSH. To disconnect, press ENTER + ~ + .\n");

    } else if (strcmp(target->alpn, "ipmi-sol") == 0) {
        bin = "ipmitool";
        exec_args[arg_idx++] = bin;
        get_username(username, sizeof(username));
        
        exec_args[arg_idx++] = "-I";
        exec_args[arg_idx++] = "lanplus";
        exec_args[arg_idx++] = "-H";
        exec_args[arg_idx++] = (char *)target->host;
        exec_args[arg_idx++] = "-U";
        exec_args[arg_idx++] = username;
        exec_args[arg_idx++] = "-p";
        exec_args[arg_idx++] = port_str;
        exec_args[arg_idx++] = "sol";
        exec_args[arg_idx++] = "activate";

    } else if (strcmp(target->alpn, "telnet") == 0) {
        bin = "telnet";
        exec_args[arg_idx++] = bin;
        exec_args[arg_idx++] = (char *)target->host;
        exec_args[arg_idx++] = port_str;
    } else {
        fprintf(stderr, "[ERROR] Unsupported protocol: '%s'\n", target->alpn);
        exit(1);
    }

    /* Array must be NULL terminiated for execvp. */
    exec_args[arg_idx] = NULL;

    /*
    fprintf(stderr, "[DEBUG] Executing command construction via execvp:\n -> ");
    for (int i = 0; exec_args[i] != NULL; i++) {
        fprintf(stderr, "%s ", exec_args[i]);
    }
    fprintf(stderr, "\n\n");
    */

    execvp(bin, exec_args);

    /* Note that we are not calling fork and the exec'd program replaces
     * this one at this point, so we should never get past here. This is what
     * we want at least for now, and exiting the resulting program will cause
     * this whole thing to terminate. */
    
    perror("[ERROR] Execution failed via execvp");
    exit(1);
}

int main(int argc, char **argv) {
    /* Anything other than a single arg (including zero args) is invalid.
     * We have to get a name and if it's bogus it'll fail to resolve later */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <domain_name>\n", argv[0]);
        return 1;
    }

    /* Combine the _console prefix with the name arg to form the query name.
     * It can be absolute but it's probably easier to use relative names and
     * let the system resolver apply the configured suffix search order to
     * resolve it. */
    char qn[MAX_FIELD_LEN];
    snprintf(qn, sizeof(qn), "_console.%s", argv[1]);

    /* Resolve the name and hopefully get back a reply containing an SVCB
     * record. The function does error handling for small replies. */
    unsigned char r[NS_PACKETSZ];
    int r_len = perform_dns_lookup(qn, r, sizeof(r));

    /* Determine the number of questions and answers from the packet using
     * their well-defined offsets. Each are two bytes. */
    int q_cnt= ntohs(*(uint16_t *)(r + QUESTION_CNT_OFFSET));
    int a_cnt = ntohs(*(uint16_t *)(r + ANSWER_CNT_OFFSET));

    /* Jump past the question section and straight to the answers. */
    const unsigned char *a_ptr = skip_questions(r, r_len, q_cnt);
    if (!a_ptr) return 1;

    /* Make an empty struct to fill in with host, port, and program learned
     * from the reply to the SVCB query. */
    svc_target_t t= {0};
    if (parse_svcb_record(r, r_len, a_ptr, a_cnt, &t) != 0) {
        fprintf(stderr, "[ERROR] No SVCB record found in answers.\n");
        return 1;
    }

    /* All that for this. Execute the program determined by SVCB DNS and
     * attempt to connect to the console. */
    execute_binary(&t);

    return 0; 
}

