
limit coredumpsize 200m 

while ( 1 == 1 )

	if ( -f log/run.log.6 ) mv log/run.log.6 log/run.log.7
	if ( -f log/run.log.5 ) mv log/run.log.5 log/run.log.6
	if ( -f log/run.log.4 ) mv log/run.log.4 log/run.log.5
	if ( -f log/run.log.3 ) mv log/run.log.3 log/run.log.4
	if ( -f log/run.log.2 ) mv log/run.log.2 log/run.log.3
	if ( -f log/run.log.1 ) mv log/run.log.1 log/run.log.2
	if ( -f log/run.log ) mv log/run.log log/run.log.1

	./db/db --master >& log/run.log
	sleep 2
end
	
