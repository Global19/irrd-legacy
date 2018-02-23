#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <hdr_comm.h>
#include <pipeline_defs.h>
#include <pgp.h>

/* JW:!!!!!: these routines are not transaction compliant
   and need to be udpated later.

   if there is a crash or something else (see pgp_add) then
   there is no way to back out and restore state.
*/

static void add_list_obj   (trace_t *, char_list_t **, char_list_t **, char *);
static int  pgp_verify     (trace_t *, char *, char *, enum PGP_OP_TYPE, 
			    pgp_data_t *);
static int  pgp_sign       (trace_t *, char *, char *, char *, enum PGP_OP_TYPE);

/* global's for the execlp () call */
char *p1, *p2, *p3;

/* debug only */
void   display_pgp_data    (pgp_data_t *);

/* Execute PGP command/operation (op).
 *
 * Input:
 *   -a command to run eg, "/usr/bin/pgpv +batchmode=1 a.asc" (cmd)
 *   -pipe to send input on STDIN to (cmd)
 *    if (in) is NULL then no STDIN pipe will be created
 *   -pipe to receive output on STDOUT and STDERR from (cmd)
 *    if (out) is NULL then no STDOUT/STDERR pipe will be created
 *
 * Return:
 *   void
 *
 *   -set's the pipes (in) and (out)
 *    calling funtion should check (in) and (out) for value NULL
 *    which indicates error in executing the command
 */
void run_cmd (trace_t *tr, char *env, FILE **in, FILE **out, 
	      enum PGP_OP_TYPE op) {
  int pin[2], pout[2];

  if (in != NULL)
    *in = NULL;

  if (out != NULL)
    *out = NULL;
  
  if (in != NULL)
    pipe (pin);

  if (out != NULL)
    pipe (pout);
  
  if (fork() == 0) { /* We're the child */
    if (in != NULL) {
      close (pin[1]);
      dup2  (pin[0], 0);
      close (pin[0]);
    }
    
    if (out != NULL) {
      close (pout[0]);
      dup2  (pout[1], 1);
      dup2  (pout[1], 2);
      close (pout[1]);
    }
    
    switch (op) {
    case PGP_DEL:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "--delete-key", p1, NULL); 
      break;
    case PGP_ADD:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "-vv", "--import", p1, NULL); 
      break;
    case PGP_FINGERPR:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "--fingerprint", p1, NULL); 
      break;
    case PGP_VERIFY_REGULAR:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "-o", p1, "-v", "--decrypt", p2, NULL); 
      break;
    case PGP_VERIFY_DETACHED:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "--verify", p1, p2, NULL); 
      break;
    case PGP_SIGN_REGULAR:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "-u", p1, "--passphrase-fd", "0", "-o", p2, 
	      "--clearsign", p3, NULL);
      break;
    case PGP_SIGN_DETACHED:
      execlp (GPG, GPG, "--homedir", env, "--batch", "--yes", 
	      "-u", p1, "--passphrase-fd", "0", "-o", p2, "--armor", 
	      "--detach-sign", p3, NULL);
      break;
    default:
      printf ("Internal error: Unrecognized PGP operation (%d)\n", op);
      break;
    }

    _exit(127);
  }

  /* Only get here if we're the parent */
  if (out != NULL) {
    close (pout[1]);
    *out = fdopen (pout[0], "r");
  }
  
  if (in != NULL) {
    close (pin[0]);
    *in = fdopen (pin[1], "w");
  }
}


/* Delete a key (ie, hex id) from the rings.  
 *
 * Input:
 *   -fully qualified path to the ring directory (PGPPATH)
 *   -hex ID value of key to be removed (hex_id)
 *    (hex_id) can be in either '0x1234ABCD' format or
 *    '1234ABCD' format.  pgp_del () will do the right thing
 *    in either case.
 *
 * Return:
 *   -1 if the operation/del was a success
 *   -0 otherwise
 */
int pgp_del (trace_t *tr, char *PGPPATH, char *hex_id) {
  char buf[MAXLINE], good_hexid[16];
  int status;

  /* check for a valid hex ID */
  if (!pgp_hexid_check (hex_id, good_hexid)) {
    trace (ERROR, tr, "pgp_del () malformed hex_id (%s)\n", 
	   ((hex_id == NULL) ? "NULL" : hex_id));
    return 0;
  }

  /* set the pointer to the PATH of the rings */
  sprintf (buf, "%s", ((PGPPATH == NULL) ? "" : PGPPATH));

  /* execute the command to delete the key from PGP ... */
  p1 = good_hexid;
  run_cmd (tr, buf, NULL, NULL, PGP_DEL);

  /* check to see if the command was executed without error */
  wait (&status);

  return !status;
}

/* Add a public key to our ring.
 *
 * Input:
 *   -a fully qualified directory path to the pgp rings (PGPPATH)
 *   -fully qualified file name of the key file (key_file)
 *   -pointer to the information struct to save the hex ID, owner,
 *    and key type (ie, RSA or DSS/Diffie-Hellman) information to (pdat)
 *    If the user set's (pdat) to NULL then routine assumes the
 *    user only wants to add the key and is not interested in the
 *    hex ID or owner information.  (pdat) will be memset to all zero's 
 *    before returning the answer
 *
 * Return:
 *   -1 if the operation/add was a success
 *   -0 otherwise
 *
 *   also...
 *   -(pdat) is initialized with the hex ID, owner, and key type
 *    from the key.  If (pdat) is NULL then the routine will not gather
 *    this information.
 */
int pgp_add (trace_t *tr, char *PGPPATH, char *key_file, pgp_data_t *pdat) {
  int pgp_ok = 0;
  FILE *pgpout;
  char curline[MAXLINE];
  regex_t re, pubkey_re, owner_re;
  regmatch_t rm[2];
  char *add_ok = "^gpg:[ \t]+imported: 1";
  char *pubkey = "^gpg: key ([[:xdigit:]]{8}): public key";
  char *owner  = "^:user ID packet:[ \t]+\"(.+)\"\n$";

  /* sanity check */
  if (key_file == NULL) {
    trace (ERROR, tr, "pgp_add () NULL key file name\n");
    return 0;
  }
  trace (ERROR, tr, "pgp_add () Entering pgp_add\n");

  /* init the return hex_id and owner data stucture to all zero's */
  if (pdat != NULL)
    memset (pdat, 0, sizeof (pgp_data_t));

  /* reg ex's */
  regcomp (&re,        add_ok, REG_EXTENDED|REG_NOSUB);
  regcomp (&pubkey_re, pubkey, REG_EXTENDED);
  regcomp (&owner_re,  owner,  REG_EXTENDED);
  
  /* build the command to add 'key_file' to the public ring and
   * send it to gpg */

  /* set the pointer to the PATH of the rings */
  sprintf (curline, "%s", ((PGPPATH == NULL) ? "" : PGPPATH));
  p1 = key_file;
  run_cmd (tr, curline, NULL, &pgpout, PGP_ADD);

  /* check and see if we were able to execute the add command */
  if (pgpout == NULL) {
    trace (ERROR, tr, "pgp_add () Error in opening (%s)\n", GPG);
    return 0;
  }

  /* now parse the output: look for a successful operation from PGP
   * and collect the hex id(s) and owner(s) */
  while (fgets (curline, MAXLINE, pgpout) != NULL) {
    /* grab the hex ID */
    if (pdat != NULL &&
	0 == regexec (&pubkey_re, curline, 2, rm, 0)) {
      /* grab the hex key */
      *(curline + rm[1].rm_eo) = '\0';
      pdat->hex_cnt++;
      add_list_obj (tr, &pdat->hex_first, &pdat->hex_last,
		    (char *) (curline + rm[1].rm_so));
    }       
    /* grab the owner/uid lines */
    else if (pdat != NULL && 0 == regexec (&owner_re, curline, 2, rm, 0)) {
      *(curline + rm[1].rm_eo) = '\n'; /* need to terminate with newline */
      *(curline + rm[1].rm_eo + 1) = '\0';
      pdat->owner_cnt++;
      add_list_obj (tr, &pdat->owner_first, &pdat->owner_last,
		    (char *) (curline + rm[1].rm_so));
    }      
    /* look for an operation successful msg from pgp */
    else if (!pgp_ok &&
	     regexec (&re, curline, 0, NULL, 0) == 0)
      pgp_ok = 1;
  }
  
  /* clean up */
  fclose  (pgpout);
  regfree (&re);
  regfree (&pubkey_re);
  regfree (&owner_re);

  return (pgp_ok && (pdat == NULL || (pdat->hex_cnt && pdat->owner_cnt)));
}


/* Get the key fingerprint for key with hex id (hex_id).
 *
 * Input:
 *   -fully qualified path to the ring directory (PGPPATH)
 *   -hex ID value of key to get the fingerprint for (hex_id)
 *    (hex_id) can be in either '0x1234ABCD' format or
 *    '1234ABCD' format.  pgp_fingerpr () will do the right thing
 *    in either case.
 *   -pointer to the information struct to save the fingerprint (pdat)
 *    (pdat) will be memset to all zero's before returning the answer
 *
 * Return:
 *   -1 if the operation/del was a success
 *   -0 otherwise
 */
int pgp_fingerpr (trace_t *tr, char *PGPPATH, char *hex_id, pgp_data_t *pdat) {
  FILE *pgpout;
  char curline[MAXLINE], good_hexid[16];
  regmatch_t rm[2];
  regex_t re;
  char *fingerpr = "^[ \t]+Key fingerprint = ([[:xdigit:] ]+)\n$";

  /* sanity checks */

  /* pdat is where we return the fingerprint */
  if (pdat == NULL) { 
    trace (ERROR, tr, "pgp_fingerpr () NULL pdat struct\n");
    return 0;
  }
  
  /* check for a valid hex ID */
  if (!pgp_hexid_check (hex_id, good_hexid)) {
    trace (ERROR, tr, "pgp_fingerpr () malformed hex_id (%s)\n", 
	   ((hex_id == NULL) ? "NULL" : hex_id));
    return 0;
  }

  /* compile our reg ex */
  regcomp (&re, fingerpr, REG_EXTENDED);

  /* build the command to get the fingerprint and invoke pgp */
  sprintf (curline, "%s", ((PGPPATH == NULL) ? "" : PGPPATH));
  p1 = good_hexid;
  run_cmd (tr, curline, NULL, &pgpout, PGP_FINGERPR);

  /* sanity check */
  if (pgpout == NULL) {
    trace (ERROR, tr, "pgp_fingerpr () NULL pgpout, couldn't open (%s)\n", PGPK);
    return 0;
  }

  /* init the fingerprint return data stucture to all zero's */
  memset (pdat, 0, sizeof (pgp_data_t));

  /* parse the gpg output and get the fingerprint */
  while (fgets (curline, MAXLINE, pgpout) != NULL) {
    /* pick off the key fingerprint */
    if (regexec (&re, curline, 2, rm, 0) == 0) {
      *(curline + rm[1].rm_eo) = '\0';
      pdat->fingerpr_cnt++;
      add_list_obj (tr, &pdat->fp_first, &pdat->fp_last,
		    (char *) (curline + rm[1].rm_so));
    }
  }

  /* clean up */
  fclose  (pgpout);
  regfree (&re);

  /* set the hex ID if the key was found in our ring */
  if (pdat->fingerpr_cnt > 0) {
    pdat->hex_cnt++;
    add_list_obj (tr, &pdat->hex_first, &pdat->hex_last, &good_hexid[2]);
    return 1;
  }
  /* make sure we are not returning any info if we didn't get a fingerpr */
  else {
    memset (pdat, 0, sizeof (pgp_data_t));
    return 0;
  }
}

/* Verify the file named (*vinfile).  This function assumes the file
 * is not encrypted and was signed with a regular signature (ie, not detached).
 * The hex id is collected and the pgpv output is sent to file 
 *    (voutfile)
 * (ie, the (*vinfile) with the signature removed)
 *
 * Input:
 *   -a fully qualified directory path to the pgp rings (PGPPATH)
 *   -fully qualified file name of the signed file (vinfile)
 *   -a file name for the pgpv output to be collected (voutfile)
 *   -pointer to the information struct to save the hex ID (pdat)
 *    (pdat) will be memset to all zero's before returning the answer
 *
 * Warning:
 *   The calling function needs to be sure that pgpv has permissions to 
 *   write to (voutfile).  If there is a problem then a properly signed
 *   key might be mistaken as a bad signature as this function will return
 *   0, ie, could not verify (vinfile).
 *
 * Return:
 *   -1 if the operation/verify was a success
 *   -0 otherwise
 *
 *   also...
 *   -(pdat) is initialized with the hex ID
 *   -(voutfile) is set to the vinfile with the signature removed
 */
int pgp_verify_regular (trace_t *tr, char *PGPPATH, char *vinfile, 
			char *voutfile, pgp_data_t *pdat) {
  char env[MAXLINE+1];

  /* sanity checks */
  if (vinfile == NULL) {
    trace (ERROR, tr, "pgp_verify_regular () NULL key file name\n");
    return 0;
  }

  /* pdat is where we return the hex id and owner who signed the file */
  if (pdat == NULL) { 
    trace (ERROR, tr, "pgp_verify_regular () NULL pdat struct\n");
    return 0;
  }

  if (voutfile == NULL) { 
    trace (ERROR, tr, "pgp_verify_regular () NULL pgpv output file name\n");
    return 0;
  }

  /* build the command to verify (vinfile) */
  snprintf (env, MAXLINE, "%s", ((PGPPATH == NULL) ? "" : PGPPATH));
  p1 = voutfile;
  p2 = vinfile;

  return pgp_verify (tr, "pgp_verify_regular", env, PGP_VERIFY_REGULAR, pdat);
}


/* Verify the file named (vinfile) using the detached signature file (vsigfile).  
 * This function makes no assumes about the names of the input file.  The function
 * will take care of any naming issues.  The hex id is collected and initialized 
 * in (pdat).
 *
 * pgp_verify_detached () does not make any assumptions about the names
 * of the pgp files (vinfile) and (vsigfile).  The function will first
 * see if (vsigfile) is named (vinfile).asc.  If not it will try to create a
 * link from (vsigfile) to (vinfile).asc.  If that fails it will try to
 * create links in /var/tmp.
 *
 * The last and obvious consideration is pgp_verify_detached () must have
 * read permissions for (vinfile) and (vsigfile)
 *
 * Input:
 *   -a fully qualified directory path to the pgp rings (PGPPATH)
 *   -fully qualified file name of the file to be verified  (vinfile)
 *   -fully qualified file name of the detached signature file (vsigfile)
 *   -pointer to the information struct to save the hex ID and owner(s) (pdat)
 *    (pdat) will be memset to all zero's before returning the answer
 *
 * Return:
 *   -1 if the operation/verify was a success
 *   -0 otherwise
 *
 *   also...
 *   -(pdat) is initialized with the hex ID
 */
int pgp_verify_detached (trace_t *tr, char *PGPPATH, char *vinfile, 
			 char *vsigfile, pgp_data_t *pdat) {
  int  pgp_ok;
  char env[MAXLINE+1];

  /* sanity checks */
  if (vinfile == NULL) {
    trace (ERROR, tr, "pgp_verify_detached () NULL key file name\n");
    return 0;
  }

  if (vsigfile == NULL) { 
    trace (ERROR, tr, "pgp_verify_detached () NULL pgpv output file name\n");
    return 0;
  }

  /* pdat is where we return the hex id and owner of who signed the file */
  if (pdat == NULL) { 
    trace (ERROR, tr, "pgp_verify_detached () NULL pdat struct\n");
    return 0;
  }

  /* now build the command to verify (vinfile) ... */
  snprintf (env, MAXLINE, "%s", ((PGPPATH == NULL) ? "" : PGPPATH));

  /* ... and verify the detached file */
  p1 = vsigfile;
  p2 = vinfile;
  pgp_ok = pgp_verify (tr, "pgp_verify_detached", env, PGP_VERIFY_DETACHED, 
		       pdat);

  return pgp_ok;
}

/* Perform the pgp verify operation.  
 *
 * Input:
 *   -name of the calling function, used for log output (funcname)
 *   -full command line invokation (cmd)
 *
 * Return:
 *   -1 if the operation/verify was a success
 *   -0 otherwise
 *
 *   also...
 *   -(pdat) is initialized with the hex ID
 */
int pgp_verify (trace_t *tr, char *funcname, char *env, 
		enum PGP_OP_TYPE pgp_op, pgp_data_t *pdat) {
  int pgp_ok = 0;
  char curline[MAXLINE];
  FILE *pgpout = NULL;
  regmatch_t hexid_rm[2];
  regex_t pgpgood_re, hexid_re;
  char *pgpgood = "^gpg: Good signature ";
  char *hexid   = "^gpg: Signature made.*key ID ([[:xdigit:]]{8})\n$";

  /* init the return hex_id and owner data stucture to all zero's */
  memset (pdat, 0, sizeof (pgp_data_t));

  /* compile our regex's */
  regcomp (&pgpgood_re, pgpgood,  REG_EXTENDED|REG_NOSUB);
  regcomp (&hexid_re,   hexid,    REG_EXTENDED);

  /* check for problems executing the verify command */
  run_cmd (tr, env, NULL, &pgpout, pgp_op);
  if (pgpout == NULL) {
    trace (ERROR, tr, "%s () Couldn't open gpg (%s)\n", funcname, GPG);
    return 0;
  }
  
  /* look for a successful operation from PGP and
   * collect the hex id of the signer */
  while (fgets (curline, MAXLINE, pgpout) != NULL) {
    /* look for a "Good signature ..." message */
  if (0 == regexec (&pgpgood_re, curline, 0, NULL, 0)) {
       pgp_ok = 1;
  }
    /* pick off the hex ID of the signer */
    else if (regexec (&hexid_re, curline, 2, hexid_rm, 0) == 0) {
      pdat->hex_cnt++;
      *(curline + hexid_rm[1].rm_eo) = '\0';
      add_list_obj (tr, &pdat->hex_first, &pdat->hex_last,
		    (char *) (curline + hexid_rm[1].rm_so));
    }
  }

  /* clean up */
  fclose  (pgpout);
  regfree (&pgpgood_re);
  regfree (&hexid_re);

  return pgp_ok;
}

/* Sign the file (signfile) and put the detached signature in (detsigfile).
 * A file (detsigfile) is created with the detached signature.
 *
 * Input:
 *  -a fully qualified directory path to the rings (PGPPATH)
 *  -secret pgp passphrase for signing (PGPPASS)
 *  -hex ID of the user who's key we are signing from (hex_id)
 *  -a fully qualified file name of the file to be signed (signfile)
 *  -a fully qualified file name of the file to place the detached signature
 *   (detsigfile)
 *
 * Return:
 *   -1 if the operation/signing was a success
 *   -0 otherwise
 *
 *   also...
 *   a file (detsigfile) is created with the detached signature
 */
int pgp_sign_detached (trace_t *tr, char *PGPPATH, char *PGPPASS, char *hex_id,
		       char *signfile, char *detsigfile) {
  int pgp_ok;
  char env[MAXLINE+1], gnupass[MAXLINE+1], good_hexid[16];

  /* sanity check's */
  if (signfile == NULL) {
    trace (ERROR, tr, "pgp_sign_detached () NULL signing file name\n");
    return 0;
  }
  
  if (detsigfile == NULL) { 
    trace (ERROR, tr, "pgp_sign_detached () NULL detached sig output "
	   "file name\n");
    return 0;
  }
  
  /* check for a valid hex ID */
  if (!pgp_hexid_check (hex_id, good_hexid)) {
    trace (ERROR, tr, "pgp_sign_detached () malformed hex_id (%s)\n", 
	   ((hex_id == NULL) ? "NULL" : hex_id));
    return 0;
  }

  /* build the command to sign (signfile) */
  snprintf (env, MAXLINE, "%s", (PGPPATH == NULL) ? "" : PGPPATH);

  snprintf (gnupass, MAXLINE, "%s", (PGPPASS == NULL) ? "" : PGPPASS);


  /* run the command to sign (signfile) */
  p1 = good_hexid;
  p2 = detsigfile;
  p3 = signfile;
  if (!(pgp_ok = pgp_sign (tr, "pgp_sign_detached", env, gnupass, PGP_SIGN_DETACHED)))
    remove (detsigfile);

  return pgp_ok;
}

/* Sign the file (signfile) with a regular signature an place the output
 * in (regsigfile).
 *
 * Input:
 *  -a fully qualified directory path to the rings (PGPPATH)
 *  -secret pgp passphrase for signing (PGPPASS)
 *  -hex ID of the user who's key we are signing from (hex_id)
 *  -a fully qualified file name of the file to be signed (signfile)
 *  -a fully qualified file name of the file to place the signed pgp output
 *   (regsigfile)
 *
 * Return:
 *   -1 if the operation/signing was a success
 *   -0 otherwise
 *
 *   also...
 *   a file (regsigfile) is created with the signed pgp output
 */
int pgp_sign_regular (trace_t *tr, char *PGPPATH, char *PGPPASS, char *hex_id,
		      char *signfile, char *regsigfile) {
  int pgp_ok;
  char env[MAXLINE+1], gnupass[MAXLINE+1], good_hexid[16];

  /* sanity check's */
  if (signfile == NULL) {
    trace (ERROR, tr, "pgp_sign_regular () NULL signing file name\n");
    return 0;
  }
  
  if (regsigfile == NULL) { 
    trace (ERROR, tr, "pgp_sign_regular () NULL detached sig output file name\n");
    return 0;
  }

  /* check for a valid hex ID */
  if (!pgp_hexid_check (hex_id, good_hexid)) {
    trace (ERROR, tr, "pgp_sign_regular () malformed hex_id (%s)\n", 
	   ((hex_id == NULL) ? "NULL" : hex_id));
    return 0;
  }

  /* build the command to sign (signfile) */
  snprintf (env, MAXLINE, "%s", (PGPPATH == NULL) ? "" : PGPPATH);

  snprintf (gnupass, MAXLINE, "%s", (PGPPASS == NULL) ? "" : PGPPASS);

  /* run the command to sign (signfile) */
  p1 = good_hexid;
  p2 = regsigfile;
  p3 = signfile;
  if (!(pgp_ok = pgp_sign (tr, "pgp_sign_regular", env, gnupass, PGP_SIGN_REGULAR)))
    remove (regsigfile);

  return pgp_ok;
}

/* Perform the pgp sign operation.  
 *
 * Input:
 *   -name of the calling function, used for log output (funcname)
 *   -full command line invokation (cmd)
 *    can sign using a detached sig or regular sig
 *
 * Return:
 *   -1 if the operation/signing was a success
 *   -0 otherwise
 *
 *   also...
 *   -the output file specified in (cmd) is set with the detached
 *    signature or regular signature output
 */
int pgp_sign (trace_t *tr, char *funcname, char *env, char *passwd, 
	      enum PGP_OP_TYPE pgp_op) {
  int status;
  FILE *pgpin = NULL;

  /* execute the command to sign the file ... */
  run_cmd (tr, env, &pgpin, NULL, pgp_op);

  /* check to see if the command was executed without error */
  if (pgpin == NULL) {
    trace (ERROR, tr, "%s () Error, could not execute sign command\n", funcname);
    return 0;
  }

  /* send the passphrase */
  fprintf (pgpin, "%s", passwd);
  fclose (pgpin);

  /* get the return code see if the command was executed without error */
  wait (&status);

  /* a "0" return code means the command executed without error */
  return !status;
}


/* Add (key) to the linked list point to by (start).
 *
 * Input:
 *   -a pointer to a linked list of keys (ie, hex id's or owners/uids or 
 *    key fingerprints) (start)
 *   -a point to the end of the linked list of keys (last)
 *    makes it easier to add new keys
 *   -char string of the key to add to the linked list (key)
 *
 * Return:
 *   void
 *
 *   function add's (key) to the end of the linked list
 */
void add_list_obj (trace_t *tr, char_list_t **start, char_list_t **last, char *key) {
  char_list_t *obj;

  /* create a new object and initialize the key member ... */
  obj = (char_list_t *) malloc (sizeof (char_list_t));
  obj->next = NULL;
  obj->key = strdup (key);

  /* .... and add it to the end of the linked list */
  if (*start == NULL)
    *start = obj;        /* first member in list */
  else
    (*last)->next = obj; /* list already had at least 1 member */

  *last = obj;           /* make our 'last' pointer point to the last member */
}

/* Check (hexid) for a proper pgp hex ID and add a leading
 * '0x' if necessary for a well-formed hex ID.
 *
 * Input:
 *   -a hex ID to check (hexid)
 *   -char buffer to return a good pgp hex ID, ie, 0x........ (hex_out)
 *    if (hex_out) is NULL then pgp_hexid_check () will only check to 
 *    make sure (hexid) is well-formed, ie, Ox........ or ........
 * 
 * Return:
 *   -1 if the (hexid) input is well-formed ie, Ox........ or ........
 *   -0 otherwise
 *
 *   -also return the good hexid in (hex_out) if (hexid) is well-formed.
 *    (hex_out) may prepend a leading '0x' if necessary
 */
int pgp_hexid_check (char *hexid, char *hex_out) {
  regex_t re;
  char *hex_num  = "^(0x)?[[:xdigit:]]{8}$";
  
  /* sanity check */
  if (hexid == NULL)
    return 0;

  regcomp (&re, hex_num, REG_EXTENDED|REG_NOSUB|REG_ICASE);
  /* see if the (hexid) is well-formed */
  if (regexec (&re, hexid, 0, NULL, 0) == 0) {
    if (hex_out != NULL) {                 /* if NULL then no initialization */
      if (*(hexid + 1) == 'x' ||
	  *(hexid + 1) == 'X')
	strcpy (hex_out, hexid);           /* perfect! */
      else
	sprintf (hex_out, "0x%s", hexid);  /* need to add a leading '0x' */
    }

    return 1;
  }
  
  /* the hexid was ill-formed */
  return 0;
}

/* Free memory in the 'pdat' data structure.
 *
 * Input:
 *  -a pointer to the 'pdat' struct (pdat)
 *
 * Return:
 *  -void
 */
void pgp_free (pgp_data_t *pdat) {
  char_list_t *p;

  /* free the hex ID list */
  for (p = pdat->hex_first; p != NULL; p = p->next)
    free (p->key);

  /* free the owner/uid list */
  for (p = pdat->owner_first; p != NULL; p = p->next)
    free (p->key);

  /* free the fingerpr list */
  for (p = pdat->fp_first; p != NULL; p = p->next)
    free (p->key);
}

/* debug only.  print out our 3 linked lists: hex id, owner and fingerprint */
void display_pgp_data (pgp_data_t *pdat) {
  char_list_t *p;

  printf ("\n\n----------\n\n");
  
  printf ("key type (%d) signing key (%d)\n", pdat->type, pdat->sign_key);

  printf ("hex data:     count (%d)\n", pdat->hex_cnt);
  for (p = pdat->hex_first; p != NULL; p = p->next)
    printf ("%s\n", p->key);
  printf ("\n");

  printf ("owner data:   count (%d)\n", pdat->owner_cnt);
  for (p = pdat->owner_first; p != NULL; p = p->next)
    printf ("%s\n", p->key);
  printf ("\n");
    
  printf ("fingerpr data: count (%d)\n", pdat->fingerpr_cnt);
  for (p = pdat->fp_first; p != NULL; p = p->next)
    printf ("%s\n", p->key);
  printf ("\n");

  printf ("\n----------\n\n");
}

/* Code junk yard.  Will we need it again someday? */

  /* JW: this is the old pgp_add () forking code
     exec the pgpk command and add the key to our ring 
  pipe(p);
  if (fork() == 0) {
    dup2 (p[0], 1);
    dup2 (p[0], 2);
    close (p[1]);
    
    execlp (PGPK, PGPK, "--batchmode=1", "-a", key_file, NULL);
    
    trace (ERROR, tr, "pgp_add () child: oops! shouldn't be here, execlp fail\n");    
    _exit(127);
  }

  close(p[0]);
  if ((pgpout = fdopen (p[1], "r")) == NULL) {
    trace (ERROR, tr, "pgp_add ()  Couldn't open pipe: update_pgp_ring");
    return 0;
  }
  */

/* setenv code 
  * set the PGPPATH environment var so PGP can find the rings *
  if (PGPPATH != NULL) {
    sprintf (curline, "PGPPATH=%s", PGPPATH);
    if (putenv (curline)) {
      trace (ERROR, tr, "%s () Couldn't set envir var (%s)\n",funcname, PGPPATH);
      return 0;
    }
  }
*/


/* run_cmd code

void run_cmd_orig (char *cmd, FILE **in, FILE **out) {
  int pin[2], pout[2];

  if (in != NULL)
    *in = NULL;

  if (out != NULL)
    *out = NULL;
  
  if (in != NULL)
    pipe (pin);

  if (out != NULL)
    pipe (pout);
  
  if (fork() == 0) { 
    if (in != NULL) {
      close (pin[1]);
      dup2  (pin[0], 0);
      close (pin[0]);
    }
    
    if (out != NULL) {
      close (pout[0]);
      dup2  (pout[1], 1);
      dup2  (pout[1], 2);
      close (pout[1]);
    }
    
    execl("/bin/sh", "sh", "-c", cmd, NULL);
    _exit(127);
  }

  if (out != NULL) {
    close (pout[1]);
    *out = fdopen (pout[0], "r");
  }
  
  if (in != NULL) {
    close (pin[0]);
    *in = fdopen (pin[1], "w");
  }
}

*/
