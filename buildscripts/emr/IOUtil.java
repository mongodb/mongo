// IOUtil.java

import java.io.*;
import java.net.*;
import java.util.*;

public class IOUtil {

    public static String urlFileName( String url ) {
        int idx = url.lastIndexOf( "/" );
        if ( idx < 0 )
            return url;
        return url.substring( idx + 1 );
    }

    public static long pipe( InputStream in , OutputStream out )
        throws IOException {

        long bytes = 0;

        byte[] buf = new byte[2048];
        
        while ( true ) {
            int x = in.read( buf );
            if ( x < 0 ) 
                break;

            bytes += x;
            out.write( buf , 0 , x );
        }

        return bytes;
    }

    public static class PipingThread extends Thread {
        public PipingThread( InputStream in , OutputStream out ) {
            _in = in;
            _out = out;

            _wrote = 0;
        }
        
        public void run() {
            try {
                _wrote = pipe( _in , _out );
            }
            catch ( IOException ioe ) {
                ioe.printStackTrace();
                _wrote = -1;
            }
        }
        
        public long wrote() {
            return _wrote;
        }

        long _wrote;
        
        final InputStream _in;
        final OutputStream _out;
    }

    public static String readStringFully( InputStream in ) 
        throws IOException {
        
        ByteArrayOutputStream bout = new ByteArrayOutputStream();
        pipe( in , bout );
        return new String( bout.toByteArray() , "UTF8" );
        
    }

    public static Map<String,Object> readPythonSettings( File file ) 
        throws IOException {
        
        String all = readStringFully( new FileInputStream( file ) );

        Map<String,Object> map = new TreeMap<String,Object>();
        
        for ( String line : all.split( "\n" ) ) {
            line = line.trim();
            if ( line.length() == 0 )
                continue;
            
            String[] pcs = line.split( "=" );
            if ( pcs.length != 2 )
                continue;
            
            String name = pcs[0].trim();
            String value = pcs[1].trim();
            
            if ( value.startsWith( "\"" ) ) {
                map.put( name , value.substring( 1 , value.length() - 1 ) );
            }
            else {
                map.put( name , Long.parseLong( value ) );
            }
            
        }
        
        return map;
    }

    public static String[] runCommand( String cmd , File dir )
        throws IOException {
        
        Process p = Runtime.getRuntime().exec( cmd.split( " +" ) , new String[]{} , dir );
        String[] results = new String[]{ IOUtil.readStringFully( p.getInputStream() ) , IOUtil.readStringFully( p.getErrorStream() ) };
        try {
            if ( p.waitFor() != 0 )
                throw new RuntimeException( "command failed [" + cmd + "]\n" + results[0] + "\n" + results[1] );
        }
        catch ( InterruptedException ie ) {
            throw new RuntimeException( "uh oh" );
        }
        return results;
    }


    public static void download( String http , File localDir ) 
        throws IOException {
        
        File f = localDir;
        f.mkdirs();
        
        f = new File( f.toString() + File.separator + urlFileName( http ) );
        
        System.out.println( "downloading\n\t" + http + "\n\t" + f );
        
        if ( f.exists() ) {
            System.out.println( "\t already exists" );
            return;
        }
        
        URL url = new URL( http );
        
        InputStream in = url.openConnection().getInputStream();
        OutputStream out = new FileOutputStream( f );
        
        pipe( in , out );
        
        out.close();
        in.close();
        
    }

    public static void main( String[] args )
        throws Exception {
        
        
        byte[] data = new byte[]{ 'e' , 'r' , 'h' , 0 };
        System.out.write( data );
        System.out.println( "yo" );
        
    }

}
