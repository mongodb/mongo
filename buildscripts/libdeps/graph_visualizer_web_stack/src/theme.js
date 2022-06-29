import { green, red, grey } from "@material-ui/core/colors";
import { createMuiTheme } from "@material-ui/core/styles";

// A custom theme for this app
const theme = createMuiTheme({
  palette: {
    primary: {
      light: green[300],
      main: green[500],
      dark: green[700],
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
