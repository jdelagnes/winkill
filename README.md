
# Winkill (archived)

Back to 2009, winKill is a cli application designed as a kill command for Windows.

The command is able to send a SIGINT (ctrl-break) or a SIGQUIT (ctrl-c) signal to a console detatched windows process.

# Long story short

This repository is a "fork" of [SendSignal](https://web.archive.org/web/20170305062415/http://www.latenighthacking.com/projects/2003/sendSignal/) project. Thanks to [Louis Thomas](https://web.archive.org/web/20171113142837/http://www.latenighthacking.com/louisth/) to gave us this code.

My humble contribution was to add the abaility to send the ctrl-c windows signal (SIGQUIT).
Useful, at this time, to test my shutdown hooks using Windows operating system.
