
while ( 1 == 1 )
	if ( -f log/run.log ) mv log/run.log log/run.log.1
	./db/db run >& log/run.log
end
	