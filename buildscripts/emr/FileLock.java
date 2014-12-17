// FileLock.java

import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/**
 * "locks" a resource by using the file system as storage
 * file has 1 line
 * <incarnation> <last ping time in millis>
 */
public class FileLock {
    
    public FileLock( String logicalName ) 
        throws IOException {

        _file = new File( "/tmp/java-fileLock-" + logicalName );
        _incarnation = "xxx" + Math.random() + "yyy";

        if ( ! _file.exists() ) {
            FileOutputStream fout = new FileOutputStream( _file );
            fout.write( "\n".getBytes() );
            fout.close();
        }

    }

    /**
     * takes lock
     * if someone else has it, blocks until the other one finishes
     */
    public void lock() 
        throws IOException {
        if ( _lock != null )
            throw new IllegalStateException( "can't lock when you're locked" );        
        
        try {
            _semaphore.acquire();
        }
        catch ( InterruptedException ie ) {
            throw new RuntimeException( "sad" , ie );
        }

        _raf = new RandomAccessFile( _file , "rw" );
        _lock = _raf.getChannel().lock();
    }

    public void unlock() 
        throws IOException {

        if ( _lock == null )
            throw new IllegalStateException( "can't unlock when you're not locked" );

        _lock.release();
        _semaphore.release();
        
        _locked = false;
    }

    final File _file;
    final String _incarnation;
    
    private RandomAccessFile _raf;
    private java.nio.channels.FileLock _lock;

    private boolean _locked;

    private static Semaphore _semaphore = new Semaphore(1);


    public static void main( final String[] args ) 
        throws Exception {

        List<Thread> threads = new ArrayList<Thread>();

        for ( int i=0; i<3; i++ ) {

            threads.add( new Thread() {
                    public void run() {
                        try {
                            FileLock lock = new FileLock( args[0] );
                            
                            long start = System.currentTimeMillis();
                            
                            lock.lock();
                            System.out.println( "time to lock:\t" + (System.currentTimeMillis()-start) );
                            Thread.sleep( Integer.parseInt( args[1] ) );
                            lock.unlock();
                            System.out.println( "total time:\t" + (System.currentTimeMillis()-start) );
                        }
                        catch ( Exception e ) {
                            e.printStackTrace();
                        }
                    }
                } );
        }
        
        for ( Thread t : threads ) {
            t.start();
        }

        for ( Thread t : threads ) {
            t.join();
        }
        
    }
}
