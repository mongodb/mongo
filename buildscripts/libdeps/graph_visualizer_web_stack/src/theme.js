import { lightGreen, blueGrey, grey } from "@material-ui/core/colors";
import { createMuiTheme } from "@material-ui/core/styles";

// A custom theme for this app
const theme = createMuiTheme({
  palette: {
    primary: {
      light: lightGreen[300],
      main: lightGreen[500],
      dark: lightGreen[700],
    },
    secondary: {
      light: grey[300],
      main: grey[500],
      dark: grey[800],
      darkAccent: "#4d4d4d",
    },
    mode: "dark",
  },
});

export default theme;
