// emr.java

import java.io.*;
import java.util.*;
import java.net.*;

import org.apache.hadoop.conf.*;
import org.apache.hadoop.io.*;
import org.apache.hadoop.mapred.*;
import org.apache.hadoop.fs.*;


public class emr {

    static class MongoSuite {
        String mongo;
        String code;
        String workingDir;

        String suite;

        void copy( MongoSuite c ) {
            mongo = c.mongo;
            code = c.code;
            workingDir = c.workingDir;
            
            suite = c.suite;
            
        }

        void downloadTo( File localDir ) 
            throws IOException {
            IOUtil.download( mongo , localDir );
            IOUtil.download( code , localDir );
        }

        boolean runTest() 
            throws IOException {

            // mkdir
            File dir = new File( workingDir , suite );
            dir.mkdirs();
            
            // download
            System.out.println( "going to download" );
            downloadTo( dir );
            
            
            // explode
            System.out.println( "going to explode" );
            IOUtil.runCommand( "tar zxvf " + IOUtil.urlFileName( code ) , dir );
            String[] res = IOUtil.runCommand( "tar zxvf " + IOUtil.urlFileName( mongo ) , dir );
            for ( String x : res[0].split( "\n" ) ) {
                if ( x.indexOf( "/bin/" ) < 0 )
                    continue;
                File f = new File( dir.toString() , x );
                if ( ! f.renameTo( new File( dir , IOUtil.urlFileName( x ) ) ) )
                    throw new RuntimeException( "rename failed" );
            }
            
            List<String> cmd = new ArrayList<String>();
            cmd.add( "/usr/bin/python" );
            cmd.add( "buildscripts/smoke.py" );
            
            File log_config = new File( dir , "log_config.py" );
            System.out.println( "log_config: " + log_config.exists() );
            if ( log_config.exists() ) {

                java.util.Map<String,Object> properties = IOUtil.readPythonSettings( log_config );

                cmd.add( "--buildlogger-builder" );
                cmd.add( properties.get( "name" ).toString() );
                
                cmd.add( "--buildlogger-buildnum" );
                cmd.add( properties.get( "number" ).toString() );
                
                cmd.add( "--buildlogger-credentials" );
                cmd.add( "log_config.py" );
                
                cmd.add( "--buildlogger-phase" );
                {
                    int idx = suite.lastIndexOf( "/" );
                    if ( idx < 0 ) 
                        cmd.add( suite );
                    else
                        cmd.add( suite.substring( 0 , idx ) );
                }

            }

            cmd.add( suite );
            
            System.out.println( cmd );

            Process p = Runtime.getRuntime().exec( cmd.toArray( new String[cmd.size()] ) , new String[]{} , dir );

            List<Thread> threads = new ArrayList<Thread>();
            threads.add( new IOUtil.PipingThread( p.getInputStream() , System.out ) );
            threads.add( new IOUtil.PipingThread( p.getErrorStream() , System.out ) );

            for ( Thread t : threads ) 
                t.start();

            try {
                for ( Thread t : threads ) {
                    t.join();
                }
                int rc = p.waitFor();
                return rc == 0;
            }
            catch ( InterruptedException ie ) {
                ie.printStackTrace();
                throw new RuntimeException( "sad" , ie );
            }
            
        }

        public void readFields( DataInput in ) 
            throws IOException {
            mongo = in.readUTF();
            code = in.readUTF();
            workingDir = in.readUTF();
            
            suite = in.readUTF();
        }

        public void write( final DataOutput out ) 
            throws IOException {
            out.writeUTF( mongo );            
            out.writeUTF( code );
            out.writeUTF( workingDir );

            out.writeUTF( suite );
        }

        public String toString() {
            return "mongo: " + mongo + " code: " + code + " suite: " + suite + " workingDir: " + workingDir;
        }
    }

    public static class Map implements Mapper<Text, MongoSuite, Text, IntWritable> {

        public void map( Text key, MongoSuite value, OutputCollector<Text,IntWritable> output, Reporter reporter ) 
            throws IOException {
            
            FileLock lock = new FileLock( "mapper" );
            try {
                lock.lock();

                System.out.println( "key: " + key );
                System.out.println( "value: " + value );
                
                long start = System.currentTimeMillis();
                boolean passed = value.runTest();
                long end = System.currentTimeMillis();
                
                output.collect( new Text( passed ? "passed" : "failed" ) , new IntWritable( 1 ) );
                output.collect( new Text( key.toString() + "-time-seconds" ) , new IntWritable( (int)((end-start)/(1000)) ) );
                output.collect( new Text( key.toString() + "-passed" ) , new IntWritable( passed ? 1 : 0 ) );
                
                String ip = IOUtil.readStringFully( new URL( "http://myip.10gen.com/" ).openConnection().getInputStream() );
                ip = ip.substring( ip.indexOf( ":" ) + 1 ).trim();
                output.collect( new Text( ip ) , new IntWritable(1) );
            }
            catch ( RuntimeException re ) {
                re.printStackTrace();
                throw re;
            }
            catch ( IOException ioe ) {
                ioe.printStackTrace();
                throw ioe;
            }
            finally {
                lock.unlock();
            }
            
        }

        public void configure(JobConf job) {}
        public void close(){}
    }

    public static class Reduce implements Reducer<Text, IntWritable, Text, IntWritable> {

        public void reduce( Text key, Iterator<IntWritable> values, OutputCollector<Text,IntWritable> output , Reporter reporter ) 
            throws IOException {

            int sum = 0;
            while ( values.hasNext() ) {
                sum += values.next().get();
            }
            output.collect( key , new IntWritable( sum ) );
        }

        public void configure(JobConf job) {}
        public void close(){}
    }

    public static class MySplit implements InputSplit , Writable {
        
        public MySplit(){
        }
        
        MySplit( MongoSuite config , int length ) {
            _config = config;
            _length = length;
        }

        public long getLength() {
            return _length;
        }
        
        public String[] getLocations() {
            return new String[0];
        }

        public void readFields( DataInput in ) 
            throws IOException {
            _config = new MongoSuite();
            _config.readFields( in );
            _length = in.readInt();
        }

        public void write( final DataOutput out ) 
            throws IOException {
            _config.write( out );
            out.writeInt( _length );
        }

        MongoSuite _config;
        int _length;
    }

    public static class InputMagic implements InputFormat<Text,MongoSuite> {

        public RecordReader<Text,MongoSuite> getRecordReader( InputSplit split, JobConf job , Reporter reporter ){
            final MySplit s = (MySplit)split;
            return new RecordReader<Text,MongoSuite>() {
                
                public void close(){}
                
                public Text createKey() {
                    return new Text();
                }
                
                public MongoSuite createValue() {
                    return new MongoSuite();
                }

                public long getPos() {
                    return _seen ? 1 : 0;
                }

                public float getProgress() {
                    return getPos();
                }
                
                public boolean next( Text key , MongoSuite value ) {
                    key.set( s._config.suite );
                    value.copy( s._config );

                    
                    boolean x = _seen;
                    _seen = true;
                    return !x;
                }
                
                boolean _seen = false;
            };
        }
        
        public InputSplit[] getSplits( JobConf job , int numSplits ){
            String[] pcs = job.get( "suites" ).split(",");
            InputSplit[] splits = new InputSplit[pcs.length];
            for ( int i=0; i<splits.length; i++ ) {
                MongoSuite c = new MongoSuite();
                c.suite = pcs[i];
                
                c.mongo = job.get( "mongo" );
                c.code = job.get( "code" );
                c.workingDir = job.get( "workingDir" );

                splits[i] = new MySplit( c , 100 /* XXX */);
            }
            return splits;
        }
        
        public void validateInput(JobConf job){}

        
    }

    /**
     * args
     *   mongo tgz
     *   code tgz
     *   output path
     *   tests to run ?
     */

    public static void main( String[] args ) throws Exception{

        JobConf conf = new JobConf();
        conf.setJarByClass(emr.class);
        
        String workingDir = "/data/db/emr/";
        

        // parse args

        int pos = 0;
        for ( ; pos < args.length; pos++ ) {
            if ( ! args[pos].startsWith( "--" ) )
                break;
            
            String arg = args[pos].substring(2);
            if ( arg.equals( "workingDir" ) ) {
                workingDir = args[++pos];
            }
            else {
                System.err.println( "unknown arg: " + arg );
                throw new RuntimeException( "unknown arg: " + arg );
            }
        }
        
        String mongo = args[pos++];
        String code = args[pos++];
        String output = args[pos++];
        
        String suites = "";
        for ( ; pos < args.length; pos++ ) {
            if ( suites.length() > 0 )
                suites += ",";
            suites += args[pos];
        }
        
        if ( suites.length() == 0 )
            throw new RuntimeException( "no suites" );
        
        System.out.println( "workingDir:\t" + workingDir );
        System.out.println( "mongo:\t" + mongo );
        System.out.println( "code:\t " + code );
        System.out.println( "output\t: " + output );
        System.out.println( "suites\t: " + suites );

        if ( false ) {
            MongoSuite s = new MongoSuite();
            s.mongo = mongo;
            s.code = code;
            s.workingDir = workingDir;
            s.suite = suites;
            s.runTest();
            return;
        }

        // main hadoop set
        conf.set( "mongo" , mongo );
        conf.set( "code" , code );
        conf.set( "workingDir" , workingDir );
        conf.set( "suites" , suites );

        conf.set( "mapred.map.tasks" , "1" );
        conf.setLong( "mapred.task.timeout" , 4 * 3600 * 1000 /* 4  hours */);

        conf.setOutputKeyClass(Text.class);
        conf.setOutputValueClass(IntWritable.class);
        
        conf.setMapperClass(Map.class);
        conf.setReducerClass(Reduce.class);
        
        conf.setInputFormat(InputMagic.class);
        conf.setOutputFormat(TextOutputFormat.class);
        
        FileOutputFormat.setOutputPath(conf, new Path(output) );

        //  actually run

        JobClient.runJob( conf );
    }
}
