#!/usr/bin/python
# vim: fileencoding=utf-8
# see http://stackoverflow.com/questions/728891/correct-way-to-define-python-source-code-encoding

import os, sys, time, signal, tempfile
import re, shutil, pexpect, logging
import random, copy, glob


default_log_format = '%(asctime)s  %(funcName)15s:%(lineno)03d  %(levelname)8s | %(message)s'
default_log_datefmt = '%b %d %H:%M:%S'


class UT():
    binary = None
    test_base = None
    lockfile = None

    defaults = None

    this_site = "127.0.0.1"
    this_site_id = None

    gdb = None
    booth = None
    prompt = "CUSTOM-GDB-PROMPT-%d-%d" % (os.getpid(), time.time())

    @classmethod
    def _filename(cls, desc):
        return "/tmp/booth-unittest.%s" % desc
        return "/tmp/booth-unittest.%d.%s" % (os.getpid(), desc)


    def __init__(self, bin, dir):
        self.binary = os.path.realpath(bin)
        self.test_base = os.path.realpath(dir) + "/"
        self.defaults = self.read_test_input(self.test_base + "_defaults.txt", state="ticket")
        self.lockfile = UT._filename("lock")


    def read_test_input(self, file, state=None, m={ "ticket": {}, "message": {} } ):
        fo = open(file, "r")
        for line in fo.readlines():
            # comment?
            if re.match(r"^\s*#", line):
                continue
            # empty line
            if re.match(r"^\s*$", line):
                continue

            # message resp. ticket
            res = re.match(r"^\s*(\w+)\s*:\s*$", line)
            if res:
                state = res.group(1)
                if not m.has_key(state):
                    m[state] = {}
                continue

            assert(state)

            res = re.match(r"^\s*(\S+)\s*(.*)\s*$", line)
            if res:
                assert(state)
                if not m[state]:
                    m[state] = {}
                m[state][ res.group(1) ] = res.group(2)
        return m


    def set_state(self, kv):
        for n, v in kv.iteritems():
            self.set_val( self.translate_shorthand(n, "ticket"), v)

    def loop(self, data):
        pass


    def setup_log(self, **args):
        global default_log_format
        global default_log_datefmt

        this_test_log = logging.FileHandler( **args )
        this_test_log.setFormatter(
                logging.Formatter(fmt = default_log_format,
                    datefmt = default_log_datefmt) )
        # in the specific files we want ALL information
        this_test_log.setLevel(logging.DEBUG)
        
        this_test_log.emit(
                logging.makeLogRecord( { 
                    "msg": "## vim: set ft=messages : ##",
                    "lineno": 0,
                    "levelname": "None",
                    "level": None,} ) )

        logging.getLogger('').addHandler(this_test_log)
        return this_test_log


    def run(self):
        os.chdir(self.test_base)
        # TODO: sorted, random order
        for f in filter( (lambda f: re.match(r"^\d\d\d_.*\.txt$", f)), glob.glob("*")):
            log = None
            try:
                log = self.setup_log(filename = UT._filename(f))

                logging.warn("running test %s" % f)
                self.start_processes()

                test = self.read_test_input(f, m=copy.deepcopy(self.defaults))
                self.set_state(test["ticket"])
                self.loop(test)
            finally:
                self.stop_processes()
                if log:
                    log.close()
            return

    def run_one_test(file):
        return


    # We want shorthand in descriptions, ie. "state"
    # instead of "booth_conf->ticket[0].state".
    def translate_shorthand(self, name, context):
        if context == 'ticket':
            return "booth_conf->ticket[0]." + name
        if context == 'message':
            return "msg->" + name
        assert(False)


    def sync(self, timeout=-1):
        self.gdb.expect(self.prompt, timeout)

        answer = self.gdb.before

        # be careful not to use RE characters like +*.[] etc.
        r = '---ATH' + str(random.randint(0, 2**20)) + '---'
        self.gdb.sendline("print '" + r + "'")
        self.gdb.expect(r, timeout)
        self.gdb.expect(self.prompt, timeout)
        return answer

    def stop_processes(self):
        if os.access(self.lockfile, os.F_OK):
            os.unlink(self.lockfile)
        # In case the boothd process is already dead, isalive() would still return True
        # (because GDB still has it), but terminate() does fail.
        # So we just quit GDB, and that might take the boothd with it -
        # if not, we terminate it ourselves.
        if self.gdb:
            self.gdb.close( force=True );
        if self.booth:
            self.booth.close( force=self.booth.isalive() )

    def start_processes(self):
        self.booth = pexpect.spawn(self.binary,
                args = [ "daemon", "-D",
                    "-c", self.test_base + "/booth.conf",
                    "-s", "127.0.0.1",
                    "-l", self.lockfile,
                    ],
                env = dict( os.environ.items() +
                    [('PATH',
                        self.test_base + "/bin/:" +
                        os.getenv('PATH'))] ),
                timeout = 30,
                maxread = 32768)
        logging.info("started booth with PID %d, lockfile %s" % (self.booth.pid, self.lockfile))
        self.booth.expect(self.this_site, timeout=2)
        #print self.booth.before; exit

        self.gdb = pexpect.spawn("gdb",
                args=["-quiet",
                    "-p", str(self.booth.pid),
                    "-nx", "-nh",   # don't use .gdbinit
                    ],
                timeout = 30,
                maxread = 32768)
        logging.info("started GDB with PID %d" % self.gdb.pid)
        self.gdb.expect("(gdb)")
        self.gdb.sendline("set pagination off\n")
        self.gdb.sendline("set prompt " + self.prompt + "\\n\n");
        self.sync(2000)

        self.this_site_id = self.query_value("local->site_id")

        # do a self-test
        self.check_value("local->site_id", self.this_site_id);
        
        # Now we're set up.
        self.send_cmd("breakpoint booth_udp_send")
        self.send_cmd("breakpoint booth_udp_broadcast")
        self.send_cmd("breakpoint process_recv")


    # send a command to GDB, returning the GDB answer as string.
    def send_cmd(self, stg, timeout=-1):
        self.gdb.sendline(stg + "\n")
        return self.sync()


    def _query_value(self, which):
        val = self.send_cmd("print " + which)
        cleaned = re.search(r"^\$\d+ = (.*\S)\s*$", val, re.MULTILINE)
        assert cleaned,val
        return cleaned.group(1)

    def query_value(self, which):
        res = self._query_value(which)
        logging.debug("query_value: «%s» evaluates to «%s»" % (which, res))
        return res

    def check_value(self, which, value):
        val = self._query_value("(" + which + ") == (" + value + ")")
        logging.debug("check_value: «%s» is «%s»: %s" % (which, value, val))
        assert val == "1", val # TODO: return?

    # Send data to GDB, to inject them into the binary.
    # Handles different data types
    def set_val(self, name, value, numeric_conv=None):
        logging.debug("setting value «%s» to «%s» (num_conv %s)" %(name, value, numeric_conv))
        # string value?
        if re.match(r'^"', value):
            self.send_cmd("print strcpy(" + name + ", " + value + ")")
        # numeric
        elif numeric_conv:
            self.send_cmd("set variable " + name + " = " + numeric_conv + "(" + value + ")")
        else:
            self.send_cmd("set variable " + name + " = " + value)


class Message(UT):
    def set_break():
        "message_recv"

    # set data, with automatic htonl() for network messages.
    def send_vals(self, data):
        for n, v in data.iteritems():
            self.set_val("msg->" + n, v, "htonl")

class Ticket(UT):
    # set ticket data - 
    def send_vals(self, data):
        for (n, v) in data:
            self.set_val(n, v)


if __name__ == '__main__':
    if os.geteuid() == 0:
        sys.stderr.write("Must be run non-root; aborting.\n")
        sys.exit(1)

    # http://stackoverflow.com/questions/9321741/printing-to-screen-and-writing-to-a-file-at-the-same-time
    logging.basicConfig(filename = UT._filename('seq'),
            format = default_log_format,
            datefmt = default_log_datefmt,
            level = logging.INFO)
    console = logging.StreamHandler()
    console.setLevel(logging.WARN)
    console.setFormatter(logging.Formatter('%(levelname)-8s: %(message)s'))

    logging.getLogger('').addHandler(console)
    logging.info("Starting boothd unit tests.")

    ut = UT(sys.argv[1], sys.argv[2] + "/")
    ret = ut.run()
    sys.exit(ret)


##
##name value
##
##value.match
##
##function void():
##    tmp_path            = '/tmp/booth-tests'
##    if not os.path.exists(tmp_path):
##        os.makedirs(tmp_path)
##    test_run_path       = tempfile.mkdtemp(prefix='%d.' % time.time(), dir=tmp_path)
##
##    suite = unittest.TestSuite()
##    testclasses = [
##        SiteConfigTests,
##        #ArbitratorConfigTests,
##        ClientConfigTests,
##    ]
##    for testclass in testclasses:
##        testclass.test_run_path = test_run_path
##        suite.addTests(unittest.TestLoader().loadTestsFromTestCase(testclass))
##
##    runner_args = {
##        'verbosity' : 4,
##    }
##    major, minor, micro, releaselevel, serial = sys.version_info
##    if major > 2 or (major == 2 and minor >= 7):
##        # New in 2.7
##        runner_args['buffer'] = True
##        runner_args['failfast'] = True
##        pass
##
##    # not root anymore, so safe
##    # needed because old instances might still use the UDP port.
##    os.system("killall boothd")
##
##    runner = unittest.TextTestRunner(**runner_args)
##    result = runner.run(suite)
##
##    if result.wasSuccessful():
##        shutil.rmtree(test_run_path)
##        sys.exit(0)
##    else:
##        print "Left %s for debugging" % test_run_path
##        s
##
