import { initialState } from "./store";

export const links = (state = initialState, action) => {
  switch (action.type) {
    case "addLinks":
      return action.payload;

    default:
      return state;
  }
};

export const addLinks = (links) => ({
  type: "addLinks",
  payload: links,
});
