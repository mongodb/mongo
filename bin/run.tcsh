
while ( 1 == 1 )
	if ( -f log/run.log ) mv log/run.log log/run.log.1
	if ( -f log/run.log1 ) mv log/run.log log/run.log.2
	if ( -f log/run.log2 ) mv log/run.log log/run.log.3
	if ( -f log/run.log3 ) mv log/run.log log/run.log.4
	if ( -f log/run.log4 ) mv log/run.log log/run.log.5
	if ( -f log/run.log5 ) mv log/run.log log/run.log.6
	if ( -f log/run.log6 ) mv log/run.log log/run.log.7

	./db/db run >& log/run.log
	sleep 2
end
	